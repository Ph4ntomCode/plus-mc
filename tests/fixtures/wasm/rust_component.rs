#![no_std]
#![no_main]

use core::hint::black_box;

#[inline(never)]
fn plus_one(value: i32) -> i32 { value.wrapping_add(1) }

#[inline(never)]
fn plus_two(value: i32) -> i32 { value.wrapping_add(2) }

fn exercise(arg: i32) -> i32 {
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
    function(arg).wrapping_add(sum as i32)
}

// A deliberately non-constant loop: the component test uses its step count to
// prove on-load was mapped to this lift, not to on-unload's much cheaper one.
fn burn(iterations: u32) -> i32 {
    let mut sum = 0i32;
    let mut index = 0u32;
    while index < iterations {
        sum = sum.wrapping_add(black_box(index as i32));
        index += 1;
    }
    sum
}

#[export_name = "init-plugin"]
pub extern "C" fn init_plugin() { black_box(exercise(0)); }

#[export_name = "on-load"]
pub extern "C" fn on_load() -> i32 { black_box(burn(500)); 0 }

#[export_name = "on-unload"]
pub extern "C" fn on_unload() -> i32 { black_box(exercise(0)); 0 }

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! { loop {} }
