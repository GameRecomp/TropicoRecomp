// SPDX-License-Identifier: MIT
//! A streaming LZMA and LZMA2 decoder.
//!
//! LZMA is Igor Pavlov's public-domain algorithm: a range coder over
//! adaptive bit probabilities, literals coded with the previous byte as
//! context, and matches with four recent distances. Only the dictionary
//! (a ring buffer) is held in memory; decoded bytes go to a sink as the
//! ring fills, so a file of any size streams through.

use std::io::{self, Read};

use crate::Error;

/// Bytes are read from the compressed input through this.
struct Input<R> {
    inner: R,
    buf: Vec<u8>,
    pos: usize,
    len: usize,
    eof: bool,
    /// Bytes in the buffers before the current one.
    consumed: u64,
}

impl<R: Read> Input<R> {
    fn new(inner: R) -> Self {
        Input {
            inner,
            buf: vec![0; 1 << 16],
            pos: 0,
            len: 0,
            eof: false,
            consumed: 0,
        }
    }

    /// The next byte, or None at the end of the input.
    #[inline]
    fn byte(&mut self) -> io::Result<Option<u8>> {
        if self.pos == self.len {
            if self.eof {
                return Ok(None);
            }
            self.consumed += self.len as u64;
            self.len = self.inner.read(&mut self.buf)?;
            self.pos = 0;
            if self.len == 0 {
                self.eof = true;
                return Ok(None);
            }
        }
        let b = self.buf[self.pos];
        self.pos += 1;
        Ok(Some(b))
    }

    fn need(&mut self) -> Result<u8, Error> {
        self.byte()?
            .ok_or_else(|| Error::Corrupt("compressed data ends early".into()))
    }
}

/// The range decoder.
struct Range {
    range: u32,
    code: u32,
    /// Input bytes ran out while normalising (only valid at a stream end).
    starved: bool,
}

const PROB_INIT: u16 = 1024;

impl Range {
    fn new<R: Read>(inp: &mut Input<R>) -> Result<Range, Error> {
        if inp.need()? != 0 {
            return Err(Error::Corrupt("range coder does not start with 0".into()));
        }
        let mut code = 0u32;
        for _ in 0..4 {
            code = (code << 8) | u32::from(inp.need()?);
        }
        Ok(Range {
            range: 0xffff_ffff,
            code,
            starved: false,
        })
    }

    #[inline]
    fn normalize<R: Read>(&mut self, inp: &mut Input<R>) -> io::Result<()> {
        if self.range < 1 << 24 {
            self.range <<= 8;
            let b = match inp.byte()? {
                Some(b) => b,
                None => {
                    self.starved = true;
                    0
                }
            };
            self.code = (self.code << 8) | u32::from(b);
        }
        Ok(())
    }

    #[inline]
    fn bit<R: Read>(&mut self, p: &mut u16, inp: &mut Input<R>) -> io::Result<u32> {
        let bound = (self.range >> 11) * u32::from(*p);
        let bit = if self.code < bound {
            self.range = bound;
            *p += (2048 - *p) >> 5;
            0
        } else {
            self.range -= bound;
            self.code -= bound;
            *p -= *p >> 5;
            1
        };
        self.normalize(inp)?;
        Ok(bit)
    }

    fn tree<R: Read>(
        &mut self,
        probs: &mut [u16],
        bits: u32,
        inp: &mut Input<R>,
    ) -> io::Result<u32> {
        let mut m = 1usize;
        for _ in 0..bits {
            m = (m << 1) | self.bit(&mut probs[m], inp)? as usize;
        }
        Ok(m as u32 - (1 << bits))
    }

    fn reverse<R: Read>(
        &mut self,
        probs: &mut [u16],
        bits: u32,
        inp: &mut Input<R>,
    ) -> io::Result<u32> {
        let mut m = 1usize;
        let mut v = 0u32;
        for i in 0..bits {
            let b = self.bit(&mut probs[m], inp)?;
            m = (m << 1) | b as usize;
            v |= b << i;
        }
        Ok(v)
    }

    fn direct<R: Read>(&mut self, bits: u32, inp: &mut Input<R>) -> io::Result<u32> {
        let mut v = 0u32;
        for _ in 0..bits {
            self.range >>= 1;
            let b = u32::from(self.code >= self.range);
            if b == 1 {
                self.code -= self.range;
            }
            v = (v << 1) | b;
            self.normalize(inp)?;
        }
        Ok(v)
    }
}

/// The dictionary: a ring buffer, flushed to the sink as it fills.
struct Window {
    buf: Vec<u8>,
    pos: usize,
    /// Bytes decoded since the last dictionary reset (capped use: only
    /// whether a distance reaches before the start matters).
    filled: u64,
    flushed: usize,
}

impl Window {
    fn new(size: usize) -> Window {
        Window {
            buf: vec![0; size.max(4096)],
            pos: 0,
            filled: 0,
            flushed: 0,
        }
    }

    fn reset(&mut self) {
        self.filled = 0;
    }

    #[inline]
    fn get(&self, dist: usize) -> u8 {
        // dist >= 1 and checked against `filled` by the caller.
        let i = if self.pos >= dist {
            self.pos - dist
        } else {
            self.buf.len() + self.pos - dist
        };
        self.buf[i]
    }

    #[inline]
    fn put(
        &mut self,
        b: u8,
        sink: &mut dyn FnMut(&[u8]) -> Result<(), Error>,
    ) -> Result<(), Error> {
        self.buf[self.pos] = b;
        self.pos += 1;
        self.filled += 1;
        if self.pos == self.buf.len() {
            self.flush(sink)?;
            self.pos = 0;
            self.flushed = 0;
        }
        Ok(())
    }

    fn flush(&mut self, sink: &mut dyn FnMut(&[u8]) -> Result<(), Error>) -> Result<(), Error> {
        if self.pos > self.flushed {
            sink(&self.buf[self.flushed..self.pos])?;
            self.flushed = self.pos;
        }
        Ok(())
    }
}

struct LenDecoder {
    choice: u16,
    choice2: u16,
    low: [[u16; 8]; 16],
    mid: [[u16; 8]; 16],
    high: [u16; 256],
}

impl LenDecoder {
    fn new() -> Self {
        LenDecoder {
            choice: PROB_INIT,
            choice2: PROB_INIT,
            low: [[PROB_INIT; 8]; 16],
            mid: [[PROB_INIT; 8]; 16],
            high: [PROB_INIT; 256],
        }
    }

    /// The length minus 2.
    fn decode<R: Read>(
        &mut self,
        rc: &mut Range,
        ps: usize,
        inp: &mut Input<R>,
    ) -> io::Result<u32> {
        if rc.bit(&mut self.choice, inp)? == 0 {
            rc.tree(&mut self.low[ps], 3, inp)
        } else if rc.bit(&mut self.choice2, inp)? == 0 {
            Ok(8 + rc.tree(&mut self.mid[ps], 3, inp)?)
        } else {
            Ok(16 + rc.tree(&mut self.high, 8, inp)?)
        }
    }
}

/// lc, lp, pb.
#[derive(Clone, Copy, Debug)]
struct Props {
    lc: u32,
    lp: u32,
    pb: u32,
}

impl Props {
    fn from_byte(b: u8) -> Result<Props, Error> {
        if b >= 9 * 5 * 5 {
            return Err(Error::Corrupt(format!("LZMA properties byte {b:#x}")));
        }
        let b = u32::from(b);
        Ok(Props {
            lc: b % 9,
            lp: (b / 9) % 5,
            pb: b / 45,
        })
    }
}

/// The decoder's model and state, kept across LZMA2 chunks.
struct Model {
    props: Props,
    is_match: [u16; 192],
    is_rep: [u16; 12],
    is_rep_g0: [u16; 12],
    is_rep_g1: [u16; 12],
    is_rep_g2: [u16; 12],
    is_rep0_long: [u16; 192],
    pos_slot: [[u16; 64]; 4],
    spec_pos: [u16; 115],
    align: [u16; 16],
    len: LenDecoder,
    rep_len: LenDecoder,
    literal: Vec<u16>,
    state: usize,
    rep: [u32; 4],
}

impl Model {
    fn new(props: Props) -> Model {
        Model {
            props,
            is_match: [PROB_INIT; 192],
            is_rep: [PROB_INIT; 12],
            is_rep_g0: [PROB_INIT; 12],
            is_rep_g1: [PROB_INIT; 12],
            is_rep_g2: [PROB_INIT; 12],
            is_rep0_long: [PROB_INIT; 192],
            pos_slot: [[PROB_INIT; 64]; 4],
            spec_pos: [PROB_INIT; 115],
            align: [PROB_INIT; 16],
            len: LenDecoder::new(),
            rep_len: LenDecoder::new(),
            literal: vec![PROB_INIT; 0x300 << (props.lc + props.lp)],
            state: 0,
            rep: [0; 4],
        }
    }

    /// Decode until `limit` bytes are out (if given) or the end marker.
    /// Returns whether the end marker was seen.
    fn run<R: Read>(
        &mut self,
        rc: &mut Range,
        win: &mut Window,
        inp: &mut Input<R>,
        limit: Option<u64>,
        total: &mut u64,
        sink: &mut dyn FnMut(&[u8]) -> Result<(), Error>,
    ) -> Result<bool, Error> {
        let pb_mask = (1u64 << self.props.pb) - 1;
        let lp_mask = (1u64 << self.props.lp) - 1;
        let lc = self.props.lc;
        let start = *total;
        loop {
            if limit.is_some_and(|l| *total - start >= l) {
                return Ok(false);
            }
            if rc.starved {
                if limit.is_none() && rc.code == 0 {
                    // The input ended exactly at the end of the stream.
                    return Ok(true);
                }
                return Err(Error::Corrupt("compressed data ends early".into()));
            }
            let ps = (*total & pb_mask) as usize;
            let st = self.state;
            if rc.bit(&mut self.is_match[(st << 4) | ps], inp)? == 0 {
                // A literal.
                let prev = if win.filled > 0 {
                    u32::from(win.get(1))
                } else {
                    0
                };
                let ctx = (((*total & lp_mask) as u32) << lc) + (prev >> (8 - lc));
                let base = 0x300 * ctx as usize;
                let probs = &mut self.literal[base..base + 0x300];
                let mut sym = 1usize;
                if st >= 7 {
                    let mut mb = u32::from(win.get(self.rep[0] as usize + 1));
                    while sym < 0x100 {
                        let match_bit = ((mb >> 7) & 1) as usize;
                        mb <<= 1;
                        let b = rc.bit(&mut probs[((1 + match_bit) << 8) + sym], inp)? as usize;
                        sym = (sym << 1) | b;
                        if match_bit != b {
                            break;
                        }
                    }
                }
                while sym < 0x100 {
                    sym = (sym << 1) | rc.bit(&mut probs[sym], inp)? as usize;
                }
                win.put(sym as u8, sink)?;
                *total += 1;
                self.state = if st < 4 {
                    0
                } else if st < 10 {
                    st - 3
                } else {
                    st - 6
                };
                continue;
            }
            let len;
            if rc.bit(&mut self.is_rep[st], inp)? == 0 {
                // A match with a new distance.
                len = self.len.decode(rc, ps, inp)?;
                self.state = if st < 7 { 7 } else { 10 };
                let dist = self.distance(rc, len, inp)?;
                if dist == 0xffff_ffff {
                    return Ok(true);
                }
                self.rep = [dist, self.rep[0], self.rep[1], self.rep[2]];
            } else {
                if rc.bit(&mut self.is_rep_g0[st], inp)? == 0 {
                    if rc.bit(&mut self.is_rep0_long[(st << 4) | ps], inp)? == 0 {
                        // One byte at the last distance.
                        self.state = if st < 7 { 9 } else { 11 };
                        self.check_dist(win)?;
                        let b = win.get(self.rep[0] as usize + 1);
                        win.put(b, sink)?;
                        *total += 1;
                        continue;
                    }
                } else {
                    let dist;
                    if rc.bit(&mut self.is_rep_g1[st], inp)? == 0 {
                        dist = self.rep[1];
                    } else {
                        if rc.bit(&mut self.is_rep_g2[st], inp)? == 0 {
                            dist = self.rep[2];
                        } else {
                            dist = self.rep[3];
                            self.rep[3] = self.rep[2];
                        }
                        self.rep[2] = self.rep[1];
                    }
                    self.rep[1] = self.rep[0];
                    self.rep[0] = dist;
                }
                len = self.rep_len.decode(rc, ps, inp)?;
                self.state = if st < 7 { 8 } else { 11 };
            }
            self.check_dist(win)?;
            let dist = self.rep[0] as usize + 1;
            let mut n = len as u64 + 2;
            if let Some(l) = limit {
                let left = l - (*total - start);
                if n > left {
                    return Err(Error::Corrupt("match runs past the end of a chunk".into()));
                }
            }
            while n > 0 {
                let b = win.get(dist);
                win.put(b, sink)?;
                *total += 1;
                n -= 1;
            }
        }
    }

    fn check_dist(&self, win: &Window) -> Result<(), Error> {
        let d = u64::from(self.rep[0]) + 1;
        if d > win.filled || d as usize > win.buf.len() {
            return Err(Error::Corrupt(format!(
                "distance {d} reaches before the data"
            )));
        }
        Ok(())
    }

    fn distance<R: Read>(
        &mut self,
        rc: &mut Range,
        len: u32,
        inp: &mut Input<R>,
    ) -> io::Result<u32> {
        let ls = len.min(3) as usize;
        let slot = rc.tree(&mut self.pos_slot[ls], 6, inp)?;
        if slot < 4 {
            return Ok(slot);
        }
        let direct = (slot >> 1) - 1;
        let mut dist = (2 | (slot & 1)) << direct;
        if slot < 14 {
            // The probabilities for this slot start at dist - slot - 1,
            // and the tree index m starts at 1.
            let base = (dist - slot) as usize;
            let mut m = 1usize;
            for i in 0..direct {
                let b = rc.bit(&mut self.spec_pos[base + m - 1], inp)?;
                m = (m << 1) | b as usize;
                dist += b << i;
            }
        } else {
            dist = dist.wrapping_add(rc.direct(direct - 4, inp)? << 4);
            dist = dist.wrapping_add(rc.reverse(&mut self.align, 4, inp)?);
        }
        Ok(dist)
    }
}

/// Raw LZMA as Inno Setup stores it: five property bytes (lc/lp/pb and
/// the dictionary size), then the stream; decoded to its end marker or to
/// the end of the input. Returns the decoded size.
pub fn lzma<R: Read>(
    input: R,
    sink: &mut dyn FnMut(&[u8]) -> Result<(), Error>,
) -> Result<u64, Error> {
    let mut inp = Input::new(input);
    let props = Props::from_byte(inp.need()?)?;
    let mut d = [0u8; 4];
    for b in &mut d {
        *b = inp.need()?;
    }
    let dict = u32::from_le_bytes(d) as usize;
    if dict > 1 << 30 {
        return Err(Error::Corrupt(format!("dictionary of {dict} bytes")));
    }
    let mut win = Window::new(dict);
    let mut model = Model::new(props);
    let mut rc = Range::new(&mut inp)?;
    let mut total = 0;
    model.run(&mut rc, &mut win, &mut inp, None, &mut total, sink)?;
    win.flush(sink)?;
    Ok(total)
}

/// LZMA2 as Inno Setup stores it: one byte for the dictionary size, then
/// LZMA2 chunks up to the end marker (control byte 0). Returns the decoded
/// size.
pub fn lzma2<R: Read>(
    input: R,
    sink: &mut dyn FnMut(&[u8]) -> Result<(), Error>,
) -> Result<u64, Error> {
    let mut inp = Input::new(input);
    let db = u32::from(inp.need()?);
    if db > 40 {
        return Err(Error::Corrupt(format!("LZMA2 dictionary byte {db}")));
    }
    let dict: u64 = if db == 40 {
        0xffff_ffff
    } else {
        u64::from(2 | (db & 1)) << (db / 2 + 11)
    };
    if dict > 1 << 30 {
        return Err(Error::Corrupt(format!("dictionary of {dict} bytes")));
    }
    let mut win = Window::new(dict as usize);
    let mut model: Option<Model> = None;
    let mut total = 0u64;
    let mut first = true;
    loop {
        let c = inp.need()?;
        if c == 0 {
            break;
        }
        let hi = |inp: &mut Input<R>| -> Result<u32, Error> {
            Ok((u32::from(inp.need()?) << 8) | u32::from(inp.need()?))
        };
        if c == 1 || c == 2 {
            // Stored bytes; 1 also resets the dictionary.
            if c == 1 {
                win.reset();
            } else if first {
                return Err(Error::Corrupt(
                    "LZMA2 starts without a dictionary reset".into(),
                ));
            }
            let n = hi(&mut inp)? + 1;
            for _ in 0..n {
                let b = inp.need()?;
                win.put(b, sink)?;
                total += 1;
            }
            first = false;
            continue;
        }
        if c < 0x80 {
            return Err(Error::Corrupt(format!("LZMA2 control byte {c:#x}")));
        }
        let unpacked = ((u32::from(c) & 0x1f) << 16) + hi(&mut inp)? + 1;
        let packed = hi(&mut inp)? + 1;
        let mode = (c >> 5) & 3;
        if mode == 3 {
            win.reset();
        } else if first {
            return Err(Error::Corrupt(
                "LZMA2 starts without a dictionary reset".into(),
            ));
        }
        if mode >= 2 {
            let props = Props::from_byte(inp.need()?)?;
            if props.lc + props.lp > 4 {
                return Err(Error::Corrupt("LZMA2 lc + lp above 4".into()));
            }
            model = Some(Model::new(props));
        } else if mode == 1 {
            let props = model
                .as_ref()
                .map(|m| m.props)
                .ok_or_else(|| Error::Corrupt("LZMA2 state reset before properties".into()))?;
            model = Some(Model::new(props));
        }
        let m = model
            .as_mut()
            .ok_or_else(|| Error::Corrupt("LZMA2 chunk before properties".into()))?;
        // The chunk's packed bytes, for its own range coder.
        let consumed_before = inp_consumed(&inp);
        let mut rc = Range::new(&mut inp)?;
        m.run(
            &mut rc,
            &mut win,
            &mut inp,
            Some(u64::from(unpacked)),
            &mut total,
            sink,
        )?;
        let used = inp_consumed(&inp) - consumed_before;
        if used != u64::from(packed) || rc.starved {
            return Err(Error::Corrupt(format!(
                "LZMA2 chunk used {used} of {packed} packed bytes"
            )));
        }
        first = false;
    }
    win.flush(sink)?;
    Ok(total)
}

/// Bytes taken from `inp` so far, for chunk bookkeeping.
fn inp_consumed<R>(inp: &Input<R>) -> u64 {
    inp.consumed + inp.pos as u64
}
