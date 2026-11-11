#![no_std]
#![no_main]

use core::fmt::{self, Write};

#[link(wasm_import_module = "plusmc:host")]
extern "C" {
    fn log(pointer: *const u8, length: usize);
}

struct Buffer {
    bytes: [u8; 160],
    length: usize,
}

impl Buffer {
    fn new() -> Self {
        Self { bytes: [0; 160], length: 0 }
    }
    fn emit(&self) {
        unsafe { log(self.bytes.as_ptr(), self.length) }
    }
}

impl Write for Buffer {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        let room = self.bytes.len() - self.length;
        let count = core::cmp::min(room, text.len());
        self.bytes[self.length..self.length + count].copy_from_slice(&text.as_bytes()[..count]);
        self.length += count;
        Ok(())
    }
}

#[inline(never)]
fn plus_one(value: i32) -> i32 { value.wrapping_add(1) }

#[inline(never)]
fn plus_two(value: i32) -> i32 { value.wrapping_add(2) }

#[no_mangle]
pub extern "C" fn pmc_wasm_load(arg: i32) -> i32 {
    let function: fn(i32) -> i32 = if arg != 0 { plus_one } else { plus_two };
    let mut source = [0u8; 64];
    let mut index = 0usize;
    while index < source.len() {
        source[index] = index as u8;
        index += 1;
    }
    let mut copy = [0u8; 64];
    copy.copy_from_slice(&source);
    let mut sum = 0u32;
    for value in copy.iter() { sum = sum.wrapping_add(*value as u32); }
    let mut text = Buffer::new();
    let _ = write!(&mut text, "rust load {} {}", function(arg), sum);
    text.emit();
    0
}

#[no_mangle]
pub extern "C" fn pmc_wasm_enable() -> i32 {
    let mut text = Buffer::new();
    let _ = write!(&mut text, "rust enable");
    text.emit();
    0
}

#[no_mangle]
pub extern "C" fn pmc_wasm_disable() -> i32 {
    let mut text = Buffer::new();
    let _ = write!(&mut text, "rust disable");
    text.emit();
    0
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! { loop {} }
