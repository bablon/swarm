use std::net::{IpAddr, SocketAddr, UdpSocket};
use std::thread::{self, JoinHandle};
use std::sync::{Arc, Mutex};
use std::io::{Read, Write};
use std::collections::HashMap;
use std::fs::File;
use std::sync::mpsc;

mod tun;
use tun::tun_alloc;

use clap::Parser;
use std::net::Ipv4Addr;

#[derive(Parser)]
#[command(version, about = "Switch for L2/L3 tunnel")]
struct Cli {
    #[arg(short, long, value_name = "Address", help = "Server mode")]
    server: Option<Option<Ipv4Addr>>,

    #[arg(short, long, value_name = "Host", help = "Client mode")]
    connect: Option<Ipv4Addr>,

    #[arg(long, help = "Default port 9500")]
    tap: Option<Option<u16>>,

    #[arg(long, help = "Default port 9501")]
    tun: Option<Option<u16>>,
}

const TAP_PORT: u16 = 9500;
const TUN_PORT: u16 = 9501;

const TUN_DEV: &str = "tun0";
const TAP_DEV: &str = "tap0";
const TAP_PORT_COUNT: usize = 3;
const TAP_DEV_PAT: &str = "tap";

fn main() {
    let cli = Cli::parse();

    if cli.server.is_some() && cli.connect.is_some() {
        eprintln!("can not specify both server and client mode");
        std::process::exit(1);
    }

    if let Some(host) = cli.connect {
        let mut port: u16 = 9501;
        let mut dev: &str = TUN_DEV;
        let mut tap_mode: bool = false;

        if cli.tap.is_some() && cli.tun.is_some() {
            eprintln!("client can not specify both --tap-port and --tun-port");
            std::process::exit(1);
        }

        if let Some(val) = cli.tap {
            port = TAP_PORT;
            dev = TAP_DEV;
            tap_mode = true;
            if let Some(tap_port) = val {
                port = tap_port;
            }
        } else if let Some(val) = cli.tun {
            port = TUN_PORT;
            if let Some(tun_port) = val {
                port = tun_port;
            }
        }

        client_connect(host, port, dev, tap_mode);
        return;
    }

    let local_ipaddr = match cli.server {
        Some(Some(host)) => host,
        _ => Ipv4Addr::new(0, 0, 0, 0),
    };

    let mut tun_port = match cli.tun {
        Some(Some(port)) => Some(port),
        Some(None) => Some(TUN_PORT),
        _ => None,
    };

    let mut tap_port = match cli.tap {
        Some(Some(port)) => Some(port),
        Some(None) => Some(TAP_PORT),
        _ => None,
    };

    if tun_port.is_none() && tap_port.is_none() {
        tun_port = Some(TUN_PORT);
        tap_port = Some(TAP_PORT);
    };

    let mut tun_handle: Option<JoinHandle<_>> = None;
    let mut tap_handle: Option<JoinHandle<_>> = None;

    if let Some(port) = tun_port {
        tun_handle = Some(tun_server(local_ipaddr, port));
    }

    if let Some(port) = tap_port {
        tap_handle = Some(tap_server(local_ipaddr, port));
    }

    if let Some(handle) = tun_handle {
        handle.join().unwrap();
    }

    if let Some(handle) = tap_handle {
        handle.join().unwrap();
    }
}

fn client_connect(host: Ipv4Addr, port: u16, dev: &str, tap_mode: bool) {
        let remote = SocketAddr::new(IpAddr::V4(host), port);

        let net_rx = UdpSocket::bind(("0.0.0.0", port)).unwrap();
        let net_tx = net_rx.try_clone().unwrap();

        let mut tun_rx = tun_alloc(dev, tap_mode);
        let mut tun_tx = tun_rx.try_clone().unwrap();

        let tun_handle = thread::spawn(move || {
            let mut buf: [u8; 4096] = [0; 4096];

            loop {
                let size = tun_rx.read(&mut buf).unwrap();
                net_tx.send_to(&buf[..size], &remote).unwrap();
            }
        });

        let net_handle = thread::spawn(move || {
            let mut buf: [u8; 4096] = [0; 4096];

            loop {
                let (size, _) = net_rx.recv_from(&mut buf).unwrap();
                tun_tx.write(&buf[..size]).unwrap();
            }
        });

        net_handle.join().unwrap();
        tun_handle.join().unwrap();
}

fn tun_server(ip: Ipv4Addr, port: u16) -> JoinHandle<()> {
    let mut tun_rx = tun_alloc(TUN_DEV, false);
    let mut tun_tx = tun_rx.try_clone().unwrap();
    let net_rx = UdpSocket::bind((ip, port)).unwrap();
    let net_tx = net_rx.try_clone().unwrap();

    let client_addr_rx = Arc::new(Mutex::new(None::<SocketAddr>));
    let client_addr_tx = client_addr_rx.clone();

    thread::spawn(move || {
        let mut buf: [u8; 4096] = [0; 4096];

        loop {
            let size = tun_rx.read(&mut buf).unwrap();
            if let Some(dest) = *client_addr_tx.lock().unwrap() {
                net_tx.send_to(&buf[..size], dest).unwrap();
            }
        }
    });

    let net_handle = thread::spawn(move || {
        let mut buf: [u8; 4096] = [0; 4096];

        loop {
            let (size, dest) = net_rx.recv_from(&mut buf).unwrap();
            *client_addr_rx.lock().unwrap() = Some(dest);
            tun_tx.write(&buf[..size]).unwrap();
            println!("recv {} bytes from {}", size, dest);
        }
    });

    net_handle
}

struct Client {
    tun_tx: File,
    port_id: usize,
}

fn get_port(arr: &mut [u8; 4]) -> Option<usize> {
    for (i, n) in arr.iter_mut().enumerate() {
        if *n == 0 {
            *n = 1;
            return Some(i);
        }
    }

    return None;
}

fn put_port(arr: &mut [u8; 4], index: usize) {
    if index < arr.len() {
        arr[index] = 0;
    }
}

fn macaddr_fmt(mac: &[u8; 6]) -> String {
    format!("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5])
}

fn tap_server(ip: Ipv4Addr, port: u16) -> JoinHandle<()> {
    let net_rx = UdpSocket::bind((ip, port)).unwrap();
    let mut client_map: HashMap<[u8; 6], Client> = HashMap::new();
    let mut port_map: [u8; 4] = [0; 4];
    let (chan_tx, chan_rx) = mpsc::channel();

    let net_handle = thread::spawn(move || {
        let mut buf: [u8; 4096] = [0; 4096];

        loop {
            let (size, dest) = net_rx.recv_from(&mut buf).unwrap();
            let mac = buf[6..12].try_into().unwrap();

            let client = client_map.get_mut(&mac);

            if let Some(c) = client {
                c.tun_tx.write(&buf[..size]).unwrap();
            } else {
                if let Ok(maddr) = chan_rx.try_recv() {
                    if let Some(c) = client_map.get(&maddr) {
                        println!("tap: release port {} mac {}", c.port_id, macaddr_fmt(&maddr));
                        put_port(&mut port_map, c.port_id);
                        client_map.remove(&maddr);
                    }
                }

                if client_map.len() >= TAP_PORT_COUNT {
                    println!("drop packet from {}", dest);
                    continue;
                }

                let port_index = get_port(&mut port_map).unwrap();

                println!("tap: port {} mac {} from {}", port_index, macaddr_fmt(&mac), dest);

                let tap_dev = format!("{}{}", TAP_DEV_PAT, port_index);
                let mut tun_rx = tun_alloc(&tap_dev, true);
                let mut tun_tx = tun_rx.try_clone().unwrap();

                tun_tx.write(&buf[..size]).unwrap();

                let client = Client {
                    tun_tx: tun_tx,
                    port_id: port_index,
                };

                client_map.insert(mac, client);

                let net_tx = net_rx.try_clone().unwrap();

                let chan_tx = chan_tx.clone();

                thread::spawn(move || {
                    let mut buf: [u8; 4096] = [0; 4096];

                    loop {
                        if let Ok(size) = tun_rx.read(&mut buf) {
                            if let Ok(_) = net_tx.send_to(&buf[..size], dest) {
                                continue;
                            }
                        }

                        chan_tx.send(mac).unwrap();
                        break;
                    }
                });
            }
        }
    });

    net_handle
}
