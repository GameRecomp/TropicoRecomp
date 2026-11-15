// SPDX-License-Identifier: MIT
//! Undo the x86 call/jump transform Inno Setup applies to executables
//! before compressing them (the "CallInstructionOptimized" flag, in the
//! form used since 5.2.0).
//!
//! Setup works through a file in blocks of 64 KiB. In each block, a 0xE8
//! (call) or 0xE9 (jmp) byte whose 32-bit operand fits in the block and
//! has a high byte of 0x00 or 0xFF is taken to be an instruction. Its
//! operand's low 24 bits were made absolute (relative to the block's
//! position in the file), and its high byte was inverted when bit 23 of
//! the original operand was set, so that forward and backward targets
//! both start with 0x00.

pub const BLOCK: usize = 1 << 16;

/// Restore one block that starts at `offset` in the file.
pub fn decode_block(buf: &mut [u8], offset: u64) {
    if buf.len() < 5 {
        return;
    }
    let end = buf.len() - 4;
    let mut i = 0;
    while i < end {
        let op = buf[i];
        i += 1;
        if op != 0xe8 && op != 0xe9 {
            continue;
        }
        if buf[i + 3] == 0x00 || buf[i + 3] == 0xff {
            let addr = (offset as u32).wrapping_add(i as u32 + 4) & 0x00ff_ffff;
            let stored =
                u32::from(buf[i]) | (u32::from(buf[i + 1]) << 8) | (u32::from(buf[i + 2]) << 16);
            let rel = stored.wrapping_sub(addr);
            if rel & 0x0080_0000 != 0 {
                buf[i + 3] = !buf[i + 3];
            }
            buf[i] = rel as u8;
            buf[i + 1] = (rel >> 8) as u8;
            buf[i + 2] = (rel >> 16) as u8;
        }
        i += 4;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The encoding, written from the description above, for the tests.
    fn encode_block(buf: &mut [u8], offset: u64) {
        let end = buf.len() - 4;
        let mut i = 0;
        while i < end {
            let op = buf[i];
            i += 1;
            if op != 0xe8 && op != 0xe9 {
                continue;
            }
            if buf[i + 3] == 0x00 || buf[i + 3] == 0xff {
                let addr = (offset as u32).wrapping_add(i as u32 + 4) & 0x00ff_ffff;
                let rel = u32::from(buf[i])
                    | (u32::from(buf[i + 1]) << 8)
                    | (u32::from(buf[i + 2]) << 16);
                if rel & 0x0080_0000 != 0 {
                    buf[i + 3] = !buf[i + 3];
                }
                let v = rel.wrapping_add(addr);
                buf[i] = v as u8;
                buf[i + 1] = (v >> 8) as u8;
                buf[i + 2] = (v >> 16) as u8;
            }
            i += 4;
        }
    }

    #[test]
    fn round_trip() {
        // Forward and backward calls, a jmp, something that is not an
        // instruction (high byte 0x12), and an opcode in the last 4 bytes.
        let orig: Vec<u8> = vec![
            0x90, 0xe8, 0x10, 0x00, 0x00, 0x00, 0xe8, 0xf0, 0xff, 0xff, 0xff, 0xe9, 0x00, 0x00,
            0x80, 0x00, 0xe8, 0x01, 0x02, 0x03, 0x12, 0x90, 0xe8, 0x01, 0x02,
        ];
        for offset in [0u64, 0x1234_5678] {
            let mut b = orig.clone();
            encode_block(&mut b, offset);
            assert_ne!(b, orig);
            // Encoded high bytes are 0x00 or the untouched 0x12.
            decode_block(&mut b, offset);
            assert_eq!(b, orig);
        }
    }
}
