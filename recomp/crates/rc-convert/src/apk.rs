// SPDX-License-Identifier: MIT
//! Android packages: the shell APK from CI with the game's code and files
//! as assets, signed with APK Signature Scheme v2.
//!
//! The signature covers the whole file in three parts: the entries, the
//! central directory, and the end record (with the directory's offset
//! replaced by that of the signing block). Each part is digested in 1 MiB
//! chunks (SHA-256 of 0xa5, the chunk's length and the chunk), and the
//! chunk digests together (SHA-256 of 0x5a, their count and them). The
//! entries are digested while they are written, so a large APK is never
//! read twice. The signing block, between the entries and the central
//! directory, holds the digest, a self-signed X.509 certificate for the
//! key, the ECDSA P-256 signature and the public key.

use std::io::{self, Write};

use p256::ecdsa::signature::Signer;
use p256::ecdsa::{DerSignature, SigningKey};
use sha2::{Digest, Sha256};

const CHUNK: usize = 1 << 20;
/// APK Signature Scheme v2's block id.
const V2_ID: u32 = 0x7109_871a;
/// ECDSA with SHA-256 over 1 MiB chunks.
const ECDSA_SHA256: u32 = 0x0201;
const MAGIC: &[u8; 16] = b"APK Sig Block 42";

/// A writer that digests what passes through it in 1 MiB chunks.
pub struct Chunked<W> {
    inner: W,
    chunk: Vec<u8>,
    digests: Vec<[u8; 32]>,
}

impl<W> std::fmt::Debug for Chunked<W> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Chunked({} chunks)", self.digests.len())
    }
}

fn chunk_digest(c: &[u8]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update([0xa5]);
    h.update((c.len() as u32).to_le_bytes());
    h.update(c);
    h.finalize().into()
}

impl<W: Write> Chunked<W> {
    pub fn new(inner: W) -> Self {
        Chunked {
            inner,
            chunk: Vec::with_capacity(CHUNK),
            digests: Vec::new(),
        }
    }

    /// The digests of everything written so far (closing the last chunk).
    fn take(&mut self) -> Vec<[u8; 32]> {
        if !self.chunk.is_empty() {
            self.digests.push(chunk_digest(&self.chunk));
            self.chunk.clear();
        }
        std::mem::take(&mut self.digests)
    }

    pub fn into_inner(self) -> W {
        self.inner
    }
}

impl<W: Write> Write for Chunked<W> {
    fn write(&mut self, mut data: &[u8]) -> io::Result<usize> {
        let n = data.len();
        self.inner.write_all(data)?;
        while !data.is_empty() {
            let k = (CHUNK - self.chunk.len()).min(data.len());
            self.chunk.extend_from_slice(&data[..k]);
            data = &data[k..];
            if self.chunk.len() == CHUNK {
                self.digests.push(chunk_digest(&self.chunk));
                self.chunk.clear();
            }
        }
        Ok(n)
    }

    fn flush(&mut self) -> io::Result<()> {
        self.inner.flush()
    }
}

// ---- DER, for the certificate ----

fn der(tag: u8, content: &[u8]) -> Vec<u8> {
    let mut out = vec![tag];
    let n = content.len();
    if n < 0x80 {
        out.push(n as u8);
    } else {
        let bytes: Vec<u8> = (n as u32)
            .to_be_bytes()
            .into_iter()
            .skip_while(|&b| b == 0)
            .collect();
        out.push(0x80 | bytes.len() as u8);
        out.extend_from_slice(&bytes);
    }
    out.extend_from_slice(content);
    out
}

fn seq(parts: &[&[u8]]) -> Vec<u8> {
    der(0x30, &parts.concat())
}

const OID_ECDSA_SHA256: &[u8] = &[0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02];
const OID_EC_PUBLIC_KEY: &[u8] = &[0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01];
const OID_P256: &[u8] = &[0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07];
const OID_CN: &[u8] = &[0x06, 0x03, 0x55, 0x04, 0x03];

/// A signing key for APKs (a P-256 private key).
pub struct ApkKey(SigningKey);

impl std::fmt::Debug for ApkKey {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("ApkKey")
    }
}

impl ApkKey {
    /// From 32 random bytes (the private scalar).
    pub fn from_bytes(b: &[u8]) -> Result<ApkKey, String> {
        SigningKey::from_slice(b)
            .map(ApkKey)
            .map_err(|_| "the signing key is not a valid P-256 key".to_string())
    }

    /// SubjectPublicKeyInfo, DER.
    fn public_key(&self) -> Vec<u8> {
        let point = self.0.verifying_key().to_encoded_point(false);
        let mut bits = vec![0u8];
        bits.extend_from_slice(point.as_bytes());
        seq(&[&seq(&[OID_EC_PUBLIC_KEY, OID_P256]), &der(0x03, &bits)])
    }

    fn sign(&self, data: &[u8]) -> Vec<u8> {
        let s: DerSignature = self.0.sign(data);
        s.as_bytes().to_vec()
    }

    /// A self-signed X.509 certificate for the key.
    fn certificate(&self) -> Vec<u8> {
        let spki = self.public_key();
        let name = seq(&[&der(
            0x31,
            &seq(&[OID_CN, &der(0x0c, b"TropicoRecomp converter")]),
        )]);
        // A positive serial number from the key.
        let mut serial = Sha256::digest(&spki)[..8].to_vec();
        serial[0] &= 0x7f;
        serial[0] |= 0x01;
        let alg = seq(&[OID_ECDSA_SHA256]);
        let validity = seq(&[&der(0x17, b"250101000000Z"), &der(0x18, b"20750101000000Z")]);
        let version = der(0xa0, &der(0x02, &[2]));
        let tbs = seq(&[
            &version,
            &der(0x02, &serial),
            &alg,
            &name,
            &validity,
            &name,
            &spki,
        ]);
        let mut sig_bits = vec![0u8];
        sig_bits.extend_from_slice(&self.sign(&tbs));
        seq(&[&tbs, &alg, &der(0x03, &sig_bits)])
    }
}

fn lp(out: &mut Vec<u8>, data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_le_bytes());
    out.extend_from_slice(data);
}

/// The APK signing block for an APK whose entries were digested into
/// `entries`, with central directory `cd` and end record `end` (as if the
/// directory started where the block will).
pub fn signing_block(key: &ApkKey, entries: Vec<[u8; 32]>, cd: &[u8], end: &[u8]) -> Vec<u8> {
    let mut all = entries;
    for part in [cd, end] {
        all.extend(part.chunks(CHUNK).map(chunk_digest));
    }
    let mut h = Sha256::new();
    h.update([0x5a]);
    h.update((all.len() as u32).to_le_bytes());
    for d in &all {
        h.update(d);
    }
    let digest: [u8; 32] = h.finalize().into();

    // Signed data: digests, certificates, additional attributes.
    let mut one_digest = Vec::new();
    one_digest.extend_from_slice(&ECDSA_SHA256.to_le_bytes());
    lp(&mut one_digest, &digest);
    let mut digests = Vec::new();
    lp(&mut digests, &one_digest);
    let mut certs = Vec::new();
    lp(&mut certs, &key.certificate());
    let mut signed = Vec::new();
    lp(&mut signed, &digests);
    lp(&mut signed, &certs);
    lp(&mut signed, &[]);

    let mut one_sig = Vec::new();
    one_sig.extend_from_slice(&ECDSA_SHA256.to_le_bytes());
    lp(&mut one_sig, &key.sign(&signed));
    let mut sigs = Vec::new();
    lp(&mut sigs, &one_sig);

    let mut signer = Vec::new();
    lp(&mut signer, &signed);
    lp(&mut signer, &sigs);
    lp(&mut signer, &key.public_key());
    let mut signers = Vec::new();
    lp(&mut signers, &signer);
    let mut value = Vec::new();
    lp(&mut value, &signers);

    let mut pairs = Vec::new();
    pairs.extend_from_slice(&((4 + value.len()) as u64).to_le_bytes());
    pairs.extend_from_slice(&V2_ID.to_le_bytes());
    pairs.extend_from_slice(&value);
    let size = (pairs.len() + 8 + 16) as u64;
    let mut block = Vec::new();
    block.extend_from_slice(&size.to_le_bytes());
    block.extend_from_slice(&pairs);
    block.extend_from_slice(&size.to_le_bytes());
    block.extend_from_slice(MAGIC);
    block
}

/// Finish an APK written through `zip` (whose output digests the
/// entries): sign it and write the signing block, central directory and
/// end record.
pub fn finish<W: Write>(zip: crate::zip::ZipWriter<Chunked<W>>, key: &ApkKey) -> io::Result<W> {
    let out = zip.finish_with_block(|out, _at, cd, end| {
        let entries = out.take();
        Ok(signing_block(key, entries, cd, end))
    })?;
    Ok(out.into_inner())
}

fn le32(b: &[u8], at: usize) -> Result<usize, String> {
    b.get(at..at + 4)
        .map(|x| u32::from_le_bytes([x[0], x[1], x[2], x[3]]) as usize)
        .ok_or_else(|| "APK: truncated field".to_string())
}

/// Length-prefixed fields, one after another.
fn fields(mut b: &[u8]) -> Result<Vec<&[u8]>, String> {
    let mut out = Vec::new();
    while !b.is_empty() {
        let n = le32(b, 0)?;
        out.push(b.get(4..4 + n).ok_or("APK: truncated field")?);
        b = &b[4 + n..];
    }
    Ok(out)
}

/// Check an APK's v2 signature from scratch, as Android does: the
/// content digest over the whole file and the signature over it. Reads
/// the file in chunks.
pub fn verify<R: std::io::Read + std::io::Seek>(f: &mut R) -> Result<(), String> {
    use p256::ecdsa::signature::Verifier;
    use std::io::SeekFrom;
    let io = |e: io::Error| e.to_string();
    let len = f.seek(SeekFrom::End(0)).map_err(io)? as usize;
    let mut read_at = |at: usize, n: usize| -> Result<Vec<u8>, String> {
        let mut b = vec![0u8; n];
        f.seek(SeekFrom::Start(at as u64)).map_err(io)?;
        f.read_exact(&mut b).map_err(io)?;
        Ok(b)
    };
    let eocd_at = len.checked_sub(22).ok_or("APK: too short")?;
    let end = read_at(eocd_at, 22)?;
    if le32(&end, 0)? != 0x0605_4b50 {
        return Err("APK: no end record (or a comment after it)".into());
    }
    let cd_at = le32(&end, 16)?;
    let tail = read_at(cd_at.checked_sub(24).ok_or("APK: no signing block")?, 24)?;
    if &tail[8..] != MAGIC {
        return Err("APK: no signing block".into());
    }
    let size = u64::from_le_bytes(tail[..8].try_into().unwrap_or_default()) as usize;
    let block_at = cd_at
        .checked_sub(size + 8)
        .ok_or("APK: bad signing block size")?;
    let block = read_at(block_at, size + 8)?;
    // Find the v2 pair.
    let mut p = 8;
    let mut value = None;
    while p + 12 <= block.len() - 24 {
        let n = u64::from_le_bytes(block[p..p + 8].try_into().unwrap_or_default()) as usize;
        if le32(&block, p + 8)? == V2_ID as usize {
            value = block.get(p + 12..p + 8 + n);
        }
        p += 8 + n;
    }
    let value = value.ok_or("APK: no v2 signature")?;
    let signer = fields(
        fields(fields(value)?.first().ok_or("APK: no signers")?)?
            .first()
            .ok_or("APK: no signer")?,
    )?;
    let [signed, sigs, public] = signer[..] else {
        return Err("APK: signer".into());
    };
    let digest_entry = fields(fields(signed)?.first().ok_or("APK: digests")?)?;
    let first = digest_entry.first().ok_or("APK: no digest")?;
    if le32(first, 0)? != ECDSA_SHA256 as usize {
        return Err("APK: digest algorithm".into());
    }
    let digest = fields(&first[4..])?.first().copied().ok_or("APK: digest")?;

    // Recompute: the entries, the central directory, the end record with
    // the directory's offset replaced by the block's.
    let mut all: Vec<[u8; 32]> = Vec::new();
    let mut p = 0;
    while p < block_at {
        let n = CHUNK.min(block_at - p);
        all.push(chunk_digest(&read_at(p, n)?));
        p += n;
    }
    let cd = read_at(cd_at, eocd_at - cd_at)?;
    all.extend(cd.chunks(CHUNK).map(chunk_digest));
    let mut end = end;
    end[16..20].copy_from_slice(&(block_at as u32).to_le_bytes());
    all.push(chunk_digest(&end));
    let mut h = Sha256::new();
    h.update([0x5a]);
    h.update((all.len() as u32).to_le_bytes());
    for d in &all {
        h.update(d);
    }
    if digest != &h.finalize()[..] {
        return Err("APK: the content digest does not match".into());
    }
    let sig = fields(&fields(sigs)?.first().ok_or("APK: no signature")?[4..])?
        .first()
        .copied()
        .ok_or("APK: signature")?;
    // The public key's last 65 bytes are the uncompressed point.
    let point = public
        .get(public.len().saturating_sub(65)..)
        .ok_or("APK: public key")?;
    let vk = p256::ecdsa::VerifyingKey::from_sec1_bytes(point).map_err(|_| "APK: public key")?;
    let sig = DerSignature::try_from(sig).map_err(|_| "APK: signature encoding")?;
    vk.verify(signed, &sig)
        .map_err(|_| "APK: the signature does not verify".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn chunk_digests_match_one_pass() {
        let data: Vec<u8> = (0..(3 * CHUNK + 1234))
            .map(|i| (i * 7 % 251) as u8)
            .collect();
        let mut w = Chunked::new(Vec::new());
        for part in data.chunks(77_777) {
            w.write_all(part).unwrap();
        }
        let streamed = w.take();
        let direct: Vec<_> = data.chunks(CHUNK).map(chunk_digest).collect();
        assert_eq!(streamed, direct);
        assert_eq!(w.into_inner(), data);
    }

    fn check(apk: &[u8]) {
        verify(&mut std::io::Cursor::new(apk)).unwrap();
    }

    #[test]
    fn signed_apks_verify() {
        let key = ApkKey::from_bytes(&[42u8; 32]).unwrap();
        let mut z = crate::zip::ZipWriter::new(Chunked::new(Vec::new()));
        z.file_aligned("AndroidManifest.xml", 0o644, b"manifest", 4)
            .unwrap();
        z.file_aligned("resources.arsc", 0o644, &[1, 2, 3], 4)
            .unwrap();
        z.begin_aligned("assets/data/big.bin", 0o644, 4).unwrap();
        let big: Vec<u8> = (0..(2 * CHUNK + 99)).map(|i| (i % 253) as u8).collect();
        for part in big.chunks(100_000) {
            z.write(part).unwrap();
        }
        z.end().unwrap();
        let apk = finish(z, &key).unwrap();
        check(&apk);
        // Every entry's data starts at a multiple of 4.
        let entries = crate::zip::read_stored(&apk).unwrap();
        assert_eq!(entries.len(), 3);
        for e in &entries {
            let at = e.data.as_ptr() as usize - apk.as_ptr() as usize;
            assert_eq!(at % 4, 0, "{}", e.name);
        }
        assert_eq!(&entries[2].data[..], &big[..]);
        // A changed byte breaks the signature.
        let mut bad = apk.clone();
        bad[100] ^= 1;
        assert!(verify(&mut std::io::Cursor::new(&bad)).is_err());
    }

    #[test]
    fn the_block_is_well_formed() {
        let key = ApkKey::from_bytes(&[7u8; 32]).unwrap();
        let b = signing_block(&key, vec![[1; 32]], b"cd", b"end");
        let size = u64::from_le_bytes(b[..8].try_into().unwrap()) as usize;
        assert_eq!(b.len(), size + 8);
        assert_eq!(&b[b.len() - 16..], MAGIC);
        assert_eq!(
            u64::from_le_bytes(b[b.len() - 24..b.len() - 16].try_into().unwrap()) as usize,
            size
        );
        assert_eq!(u32::from_le_bytes(b[16..20].try_into().unwrap()), V2_ID);
        // The certificate is DER: a SEQUENCE of three.
        let c = key.certificate();
        assert_eq!(c[0], 0x30);
    }
}
