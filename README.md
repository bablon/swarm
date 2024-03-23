# Swarm

Swarm is a virtual switch based on tap/tun for L2/L3 tunnel
over UDP. It has three L2 port and one L3 port. Another version
over TCP in C is in c directory.

Server:

    sudo setup/setup-server.sh
    ./swarm

Client: (tap)

    sudo setup/setup-client.sh
    ./swarm -c serverip --tap

Client: (tun)

    ./swarm -c serverip --tun
