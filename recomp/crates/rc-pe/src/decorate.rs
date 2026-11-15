// SPDX-License-Identifier: MIT
//! Calling conventions from MSVC's C name decoration.
//!
//! Some DLLs export decorated names (`_AIL_open_stream@12`), which carry the
//! convention and, for stdcall and fastcall, the bytes of arguments the
//! callee pops. Windows system DLLs export plain names; their sizes have to
//! come from elsewhere.

use serde::Serialize;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum CallConv {
    /// `_name@N`: arguments on the stack, the callee pops `N` bytes.
    Stdcall,
    /// `@name@N`: first two arguments in ECX and EDX, the callee pops the rest.
    Fastcall,
    /// `_name`: arguments on the stack, the caller pops them.
    Cdecl,
    /// A plain name: the convention is not encoded.
    Unknown,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct Decoration<'a> {
    /// The name without decoration.
    pub name: &'a str,
    pub conv: CallConv,
    /// Argument bytes, for stdcall and fastcall.
    pub arg_bytes: Option<u32>,
}

/// Split a C-decorated name into its parts. C++ names (`?…`) and plain names
/// come back unchanged with [`CallConv::Unknown`].
pub fn undecorate(name: &str) -> Decoration<'_> {
    let plain = Decoration {
        name,
        conv: CallConv::Unknown,
        arg_bytes: None,
    };
    if let Some(rest) = name.strip_prefix('@') {
        return match sized(rest) {
            Some((n, bytes)) => Decoration {
                name: n,
                conv: CallConv::Fastcall,
                arg_bytes: Some(bytes),
            },
            None => plain,
        };
    }
    if let Some(rest) = name.strip_prefix('_') {
        return match sized(rest) {
            Some((n, bytes)) => Decoration {
                name: n,
                conv: CallConv::Stdcall,
                arg_bytes: Some(bytes),
            },
            None if !rest.is_empty() && !rest.contains('@') => Decoration {
                name: rest,
                conv: CallConv::Cdecl,
                arg_bytes: None,
            },
            None => plain,
        };
    }
    plain
}

/// `name@N` split into the name and `N`.
fn sized(rest: &str) -> Option<(&str, u32)> {
    let (n, size) = rest.rsplit_once('@')?;
    if n.is_empty() || size.is_empty() || !size.bytes().all(|c| c.is_ascii_digit()) {
        return None;
    }
    Some((n, size.parse().ok()?))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stdcall() {
        let d = undecorate("_AIL_open_stream@12");
        assert_eq!(d.name, "AIL_open_stream");
        assert_eq!(d.conv, CallConv::Stdcall);
        assert_eq!(d.arg_bytes, Some(12));
        assert_eq!(undecorate("_BinkGetError@0").arg_bytes, Some(0));
    }

    #[test]
    fn fastcall_cdecl_plain() {
        let d = undecorate("@Fast@8");
        assert_eq!(
            (d.name, d.conv, d.arg_bytes),
            ("Fast", CallConv::Fastcall, Some(8))
        );
        let d = undecorate("_printf");
        assert_eq!((d.name, d.conv), ("printf", CallConv::Cdecl));
        let d = undecorate("CreateFileA");
        assert_eq!((d.name, d.conv), ("CreateFileA", CallConv::Unknown));
        assert_eq!(undecorate("_x@").conv, CallConv::Unknown);
        assert_eq!(undecorate("?f@@YAXXZ").conv, CallConv::Unknown);
    }
}
