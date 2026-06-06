# Template only. Replace PASSWORD and storage/app paths before use.
#
# Creates a limited RouterOS API user and a credentials file expected by
# awg-routeros-app at /etc/awg-proxy/routeros-api.conf inside the container.

:local apiUser "awg-proxy"
:local apiPass "CHANGE_ME_LONG_RANDOM_PASSWORD"
:local apiAddress "172.18.0.0/24"
:local credsFile "disk1/awg-proxy/routeros-api.conf"

:if ([:len [/user/group/find where name="awg-proxy-api"]] = 0) do={
  /user/group/add name=awg-proxy-api policy=read,write,api,sensitive
}

:if ([:len [/user/find where name=$apiUser]] = 0) do={
  /user/add name=$apiUser group=awg-proxy-api password=$apiPass address=$apiAddress
} else={
  /user/set [find where name=$apiUser] group=awg-proxy-api password=$apiPass address=$apiAddress
}

/ip/service/set api disabled=no port=8728 address=$apiAddress

:do { /file/remove $credsFile } on-error={}
/file/add name=$credsFile contents=("host=172.18.0.1\nport=8728\nuser=" . $apiUser . "\npassword=" . $apiPass . "\n")

:put ("Created RouterOS API credentials at " . $credsFile)
