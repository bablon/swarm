#!/bin/bash

brctl addbr tap-br0

for i in $(seq 0 2); do
	ip tuntap add dev tap$i mode tap
	brctl addif tap-br0 tap$i
	ip link set tap$i up
done

ip addr add 10.3.5.1/24 dev tap-br0

ip link set tap-br0 up

ip tuntap add dev tun0 mode tun
ip addr add 10.3.6.1/24 dev tun0
ip link set tun0 up
