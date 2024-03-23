use nix::ioctl_write_ptr_bad;
use nix::request_code_write;
use std::fs::{OpenOptions, File};
use std::os::raw::*;
use std::os::fd::AsRawFd;

#[repr(C)]
union ifreq_ifrn {
    ifrn_name: [c_char; 16],
}

#[repr(C)]
union ifreq_ifru {
    ifru_flags: c_short,
    ifru_newname: [c_char; 16],
    ifru_data: *mut c_void,
    ifru_max: [c_uchar; 24],
}

#[repr(C)]
pub struct ifreq {
    ifr_ifrn: ifreq_ifrn,
    ifr_ifru: ifreq_ifru,
}

const TUN_IOC_MAGIC: u8 = b'T';
const TUN_IOC_TYPE_SETIFF: u8 = 202;
const IFF_TAP: i16 = 0x0002;
const IFF_TUN: i16 = 0x0001;
const IFF_NO_PI: i16 = 0x1000;

ioctl_write_ptr_bad!(
    ioctl_tunsetiff,
    request_code_write!(
        TUN_IOC_MAGIC, TUN_IOC_TYPE_SETIFF, std::mem::size_of::<i32>()
    ),
    ifreq
);

pub fn tun_alloc(dev: &str, tap: bool) -> File {
    let file = OpenOptions::new()
        .read(true).write(true).open("/dev/net/tun")
        .unwrap();

    let fd = file.as_raw_fd();
    let mut ifr = ifreq {
        ifr_ifrn: ifreq_ifrn { ifrn_name: [0; 16], },
        ifr_ifru: ifreq_ifru { ifru_max: [0; 24] },
    };

    if dev.len() == 0 || dev.len() >= 16 {
        panic!("invalid dev {}", dev);
    }

    unsafe {
        ifr.ifr_ifru.ifru_flags = IFF_NO_PI;
        ifr.ifr_ifru.ifru_flags |= if tap { IFF_TAP } else { IFF_TUN };

        for (dest, src) in ifr.ifr_ifrn.ifrn_name.iter_mut()
            .zip(dev.as_bytes().iter()) {
            *dest = *src as i8;
        }

        ioctl_tunsetiff(fd, &ifr).unwrap(); 
    }

    file
}
