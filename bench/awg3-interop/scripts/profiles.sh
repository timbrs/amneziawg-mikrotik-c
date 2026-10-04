#!/bin/sh
# Version profiles, sourced by both the server (UAPI) and the proxy (env), so
# the two ends can never drift apart in a scenario.
#
# v2 and v3 are deliberately identical except for the header protection key:
# any behavioural difference between them is then attributable to header
# protection alone, which is what makes the negative controls meaningful. v3.1
# repeats the trick one level up: it is v3 plus RandomTrailers/DisableCookies.

load_profile() {
    # AWG 3.1 switches: off everywhere except the v3.1 profile.
    RT_ON=0; DC_ON=0
    case "$1" in
    v1)
        JC=3; JMIN=40; JMAX=70
        S1=15; S2=20; S3=0; S4=0
        H1=1000000; H2=2000000; H3=3000000; H4=4000000
        I1=''; I2=''
        HPK_ON=0
        ;;
    v1.5)
        JC=3; JMIN=40; JMAX=70
        S1=15; S2=20; S3=0; S4=0
        H1=1000000; H2=2000000; H3=3000000; H4=4000000
        I1='<b 0xf1e2><r 8>'; I2='<b 0xaabb><rc 6><t>'
        HPK_ON=0
        ;;
    v2)
        JC=4; JMIN=50; JMAX=800
        S1=77; S2=41; S3=33; S4=14
        H1=1000000-1000050; H2=2000000-2000050
        H3=3000000-3000050; H4=4000000-4000050
        I1='<b 0xf1e2><r 8>'; I2=''
        HPK_ON=0
        ;;
    v3)
        JC=4; JMIN=50; JMAX=800
        S1=77; S2=41; S3=33; S4=14
        H1=1000000-1000050; H2=2000000-2000050
        H3=3000000-3000050; H4=4000000-4000050
        I1='<b 0xf1e2><r 8>'; I2=''
        HPK_ON=1
        ;;
    v3.1)
        JC=4; JMIN=50; JMAX=800
        S1=77; S2=41; S3=33; S4=14
        H1=1000000-1000050; H2=2000000-2000050
        H3=3000000-3000050; H4=4000000-4000050
        I1='<b 0xf1e2><r 8>'; I2=''
        HPK_ON=1
        RT_ON=1; DC_ON=1
        ;;
    warp)
        # What AWG configs for Cloudflare WARP look like: Cloudflare speaks
        # plain WireGuard, so S and H stay at the AmneziaWG defaults (0 and the
        # WireGuard message types 1-4) and only the junk and CPS packets sent
        # before the handshake differ. The server here is therefore plain
        # WireGuard in all but name.
        JC=4; JMIN=40; JMAX=70
        S1=0; S2=0; S3=0; S4=0
        H1=1; H2=2; H3=3; H4=4
        I1='<b 0xc2000000011419fa4bb3599f336777de><r 16>'; I2=''
        HPK_ON=0
        ;;
    *)
        echo "unknown profile: $1" >&2
        return 1
        ;;
    esac
}
