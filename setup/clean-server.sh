#!/bin/bash

ip link set tap-br0 down

for i in $(seq 0 2); do
	brctl delif tap-br0 tap$i
	ip tuntap del dev tap$i mode tap
done

brctl delbr tap-br0

ip tuntap del dev tun0 mode tun
