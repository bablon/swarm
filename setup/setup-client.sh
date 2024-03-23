#!/bin/bash

ip tuntap add dev tap0 mode tap
ip addr add 10.3.5.7/24 dev tap0
ip link set dev tap0 up

ip tuntap add dev tun0 mode tun
ip addr add 10.3.6.7/24 dev tun0
ip link set dev tun0 up
