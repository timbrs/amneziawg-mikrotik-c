// Real Noise handshakes and encrypted IP traffic; no root/TUN device needed.
package main

import (
	"bytes"
	"crypto/ecdh"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"net"
	"net/netip"
	"os"
	"os/exec"
	"strings"
	"sync"
	"time"

	"golang.zx2c4.com/wireguard/conn"
	"golang.zx2c4.com/wireguard/device"
	"golang.zx2c4.com/wireguard/tun/netstack"
)

type identity struct{ priv, pub []byte }

func gen() identity {
	k, e := ecdh.X25519().GenerateKey(rand.Reader)
	must(e)
	return identity{k.Bytes(), k.PublicKey().Bytes()}
}
func must(e error) {
	if e != nil {
		panic(e)
	}
}
func port() int {
	s, e := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	must(e)
	defer s.Close()
	return s.LocalAddr().(*net.UDPAddr).Port
}
func hexkey(k []byte) string { return hex.EncodeToString(k) }
func b64(k []byte) string    { return base64.StdEncoding.EncodeToString(k) }
func wg(id identity, ip string, cfg string) (*device.Device, *netstack.Net) {
	t, n, e := netstack.CreateNetTUN([]netip.Addr{netip.MustParseAddr(ip)}, nil, 1280)
	must(e)
	d := device.NewDevice(t, conn.NewDefaultBind(), device.NewLogger(device.LogLevelError, "wg: "))
	must(d.IpcSet("private_key=" + hexkey(id.priv) + "\n" + cfg))
	must(d.Up())
	return d, n
}
func proxy(binary string, env []string, log *os.File) func() {
	c := exec.Command(binary)
	dir := ""
	if supervisor := os.Getenv("AWG_SUPERVISOR"); supervisor != "" {
		var e error
		dir, e = os.MkdirTemp("", "awg-file-config-")
		must(e)
		fileEnv := []string{}
		for _, v := range env {
			if strings.HasPrefix(v, "AWG_CLIENT_PUBS=") {
				must(os.WriteFile(dir+"/clients.keys", []byte(strings.ReplaceAll(strings.TrimPrefix(v, "AWG_CLIENT_PUBS="), ",", "\n")+"\n"), 0600))
				fileEnv = append(fileEnv, "AWG_CLIENT_PUBS_FILE="+dir+"/clients.keys")
			} else {
				fileEnv = append(fileEnv, v)
			}
		}
		must(os.WriteFile(dir+"/route.env", []byte(strings.Join(fileEnv, "\n")+"\n"), 0600))
		c = exec.Command(supervisor, "--config-dir", dir)
		env = []string{"AWG_PROXY_BIN=" + binary}
	}
	for _, v := range os.Environ() {
		if !strings.HasPrefix(v, "AWG_") {
			c.Env = append(c.Env, v)
		}
	}
	c.Env = append(c.Env, env...)
	c.Stdout = log
	c.Stderr = log
	must(c.Start())
	return func() {
		c.Process.Signal(os.Interrupt)
		c.Wait()
		if dir != "" {
			os.RemoveAll(dir)
		}
	}
}
func run(profile int, binary string) {
	server := gen()
	clients := []identity{gen(), gen(), gen()}
	wp, sp, np := port(), port(), port()
	scfg := fmt.Sprintf("listen_port=%d\n", wp)
	pubs := []string{}
	for i, c := range clients {
		scfg += fmt.Sprintf("public_key=%s\nallowed_ip=10.66.0.%d/32\n", hexkey(c.pub), i+2)
		pubs = append(pubs, b64(c.pub))
	}
	sd, sn := wg(server, "10.66.0.1", scfg)
	defer sd.Close()
	echo, e := sn.ListenUDPAddrPort(netip.MustParseAddrPort("10.66.0.1:9000"))
	must(e)
	defer echo.Close()
	go func() {
		b := make([]byte, 2048)
		for {
			n, a, e := echo.ReadFrom(b)
			if e != nil {
				return
			}
			echo.WriteTo(b[:n], a)
		}
	}()
	common := []string{"AWG_JC=0", "AWG_JMIN=0", "AWG_JMAX=0", "AWG_H1=101", "AWG_H2=102", "AWG_H3=103", "AWG_H4=104", "AWG_SPIN=0", "AWG_SOCKET_BUF=1048576", "AWG_SERVER_PUB=" + b64(server.pub), "AWG_CLIENT_PUBS=" + strings.Join(pubs, ",")}
	if os.Getenv("TEST_NO_OFFLOAD") == "1" {
		common = append(common, "AWG_NO_GRO=1", "AWG_NO_GSO=1")
	}
	if profile == 1 {
		common = append(common, "AWG_S1=0", "AWG_S2=0")
	} else {
		common = append(common, "AWG_S1=13", "AWG_S2=17", "AWG_S3=19", "AWG_S4=16")
	}
	if profile >= 3 {
		common = append(common, "AWG_HEADER_PROTECTION_KEY="+b64(bytes.Repeat([]byte{42}, 32)))
	}
	if profile == 4 {
		common = append(common, "AWG_RANDOM_TRAILERS=1")
	}
	log, e := os.CreateTemp("", "awg-multi-interop-*.log")
	must(e)
	defer func() { log.Close(); os.Remove(log.Name()) }()
	defer func() {
		if x := recover(); x != nil {
			b, _ := os.ReadFile(log.Name())
			fmt.Fprintln(os.Stderr, string(b))
			panic(x)
		}
	}()
	se := append(append([]string{}, common...), "AWG_MODE=server", fmt.Sprintf("AWG_LISTEN=127.0.0.1:%d", sp), fmt.Sprintf("AWG_REMOTE=127.0.0.1:%d", wp))
	stopServer := proxy(binary, se, log)
	defer stopServer()
	ne := append(append([]string{}, common...), "AWG_MODE=normal", fmt.Sprintf("AWG_LISTEN=127.0.0.1:%d", np), fmt.Sprintf("AWG_REMOTE=127.0.0.1:%d", sp))
	stopNormal := proxy(binary, ne, log)
	defer stopNormal()
	time.Sleep(200 * time.Millisecond)
	var group sync.WaitGroup
	errors := make(chan error, len(clients))
	for i, c := range clients {
		d, n := wg(c, fmt.Sprintf("10.66.0.%d", i+2), fmt.Sprintf("public_key=%s\nallowed_ip=10.66.0.1/32\nendpoint=127.0.0.1:%d\n", hexkey(server.pub), np))
		defer d.Close()
		group.Add(1)
		go func(i int, n *netstack.Net) {
			defer group.Done()
			s, e := n.DialUDPAddrPort(netip.AddrPort{}, netip.MustParseAddrPort("10.66.0.1:9000"))
			if e != nil {
				errors <- e
				return
			}
			defer s.Close()
			for j := 0; j < 100; j++ {
				data := bytes.Repeat([]byte{byte(i + 1)}, 1024)
				copy(data, fmt.Sprintf("client%d-packet%04d", i, j))
				s.SetDeadline(time.Now().Add(8 * time.Second))
				if _, e = s.Write(data); e != nil {
					errors <- e
					return
				}
				b := make([]byte, 2048)
				n, e := s.Read(b)
				if e != nil {
					errors <- fmt.Errorf("profile %d client %d packet %d: %w", profile, i, j, e)
					return
				}
				if !bytes.Equal(data, b[:n]) {
					errors <- fmt.Errorf("payload misdelivery client %d packet %d", i, j)
					return
				}
			}
		}(i, n)
	}
	group.Wait()
	close(errors)
	for e := range errors {
		must(e)
	}
	fmt.Printf("PASS real WireGuard: profile=%d, 3 independent keys, one normal port, 300 encrypted 1024-byte round trips\n", profile)
}
func main() {
	binary := os.Getenv("AWG_REAL_PROXY")
	if binary == "" {
		panic("set AWG_REAL_PROXY")
	}
	for p := 1; p <= 4; p++ {
		run(p, binary)
	}
}
