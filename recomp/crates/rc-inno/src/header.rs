// SPDX-License-Identifier: MIT
//! The setup header and its entries, as Inno Setup 5.5.0 (Unicode) writes
//! them.
//!
//! Each structure is a run of length-prefixed strings (UTF-16 in the
//! Unicode build, except a few that are always bytes) followed by fixed
//! fields. Only what extraction needs is kept, but every field is read, so
//! that the next structure starts where it should.

use crate::Error;

/// How the file data's chunks are compressed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Compression {
    Stored,
    Zlib,
    Bzip2,
    Lzma,
    Lzma2,
}

/// A file the installer writes.
#[derive(Debug, Clone)]
pub struct FileEntry {
    /// The destination as the script wrote it, e.g. `{app}\data2\x.cfg`.
    pub dest: String,
    /// Index into the locations, or None for a file not in the installer
    /// (an external file, or the uninstaller).
    pub location: Option<u32>,
    /// The uninstaller program, which Setup writes itself.
    pub uninstaller: bool,
    pub components: String,
    pub languages: String,
}

/// Where a stored file's data is, and how to check it.
#[derive(Debug, Clone)]
pub struct Location {
    pub first_slice: u32,
    pub last_slice: u32,
    /// Offset of the chunk from the start of the file data.
    pub chunk_offset: u32,
    /// Offset of the file in the decompressed chunk.
    pub offset: u64,
    pub size: u64,
    /// Size of the chunk's compressed data (after its magic).
    pub chunk_size: u64,
    pub sha1: [u8; 20],
    /// Windows FILETIME: 100 ns units since 1601.
    pub timestamp: u64,
    pub flags: u16,
}

impl Location {
    pub const CALL_FILTER: u16 = 1 << 4;
    pub const ENCRYPTED: u16 = 1 << 6;
    pub const COMPRESSED: u16 = 1 << 7;

    pub fn has(&self, flag: u16) -> bool {
        self.flags & flag != 0
    }
}

#[derive(Debug, Clone)]
pub struct Setup {
    /// The ID of the setup data, e.g. `Inno Setup Setup Data (5.5.0) (u)`.
    pub version: String,
    pub app_name: String,
    pub app_version: String,
    pub compression: Compression,
    pub files: Vec<FileEntry>,
    /// Directories the installer creates.
    pub dirs: Vec<String>,
    pub locations: Vec<Location>,
}

struct Reader<'a> {
    d: &'a [u8],
    p: usize,
}

impl<'a> Reader<'a> {
    fn bytes(&mut self, n: usize) -> Result<&'a [u8], Error> {
        if self.d.len() - self.p < n {
            return Err(Error::Corrupt(format!(
                "setup header ends at {} reading {n} bytes",
                self.p
            )));
        }
        let b = &self.d[self.p..self.p + n];
        self.p += n;
        Ok(b)
    }

    fn u8(&mut self) -> Result<u8, Error> {
        Ok(self.bytes(1)?[0])
    }

    fn u16(&mut self) -> Result<u16, Error> {
        let b = self.bytes(2)?;
        Ok(u16::from_le_bytes([b[0], b[1]]))
    }

    fn u32(&mut self) -> Result<u32, Error> {
        let b = self.bytes(4)?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn i32(&mut self) -> Result<i32, Error> {
        Ok(self.u32()? as i32)
    }

    fn u64(&mut self) -> Result<u64, Error> {
        Ok(u64::from(self.u32()?) | (u64::from(self.u32()?) << 32))
    }

    fn skip(&mut self, n: usize) -> Result<(), Error> {
        self.bytes(n).map(|_| ())
    }

    /// A string of bytes.
    fn ansi(&mut self) -> Result<&'a [u8], Error> {
        let n = self.u32()? as usize;
        self.bytes(n)
    }

    /// A UTF-16 string.
    fn wide(&mut self) -> Result<String, Error> {
        let b = self.ansi()?;
        if b.len() % 2 != 0 {
            return Err(Error::Corrupt(format!(
                "odd-sized string before {}",
                self.p
            )));
        }
        let units: Vec<u16> = b
            .as_chunks::<2>()
            .0
            .iter()
            .map(|&c| u16::from_le_bytes(c))
            .collect();
        Ok(String::from_utf16_lossy(&units))
    }

    fn wides(&mut self, n: usize) -> Result<Vec<String>, Error> {
        (0..n).map(|_| self.wide()).collect()
    }

    /// MinVersion and OnlyBelowVersion: (Windows, NT, NT service pack) each.
    fn versions(&mut self) -> Result<(), Error> {
        self.skip(2 * (4 + 4 + 2))
    }

    fn count(&mut self) -> Result<usize, Error> {
        let n = self.i32()?;
        if !(0..=1_000_000).contains(&n) {
            return Err(Error::Corrupt(format!("{n} entries")));
        }
        Ok(n as usize)
    }
}

pub fn parse(version: &str, header: &[u8], locations: &[u8]) -> Result<Setup, Error> {
    let mut r = Reader { d: header, p: 0 };

    // AppName, AppVerName, AppId, AppCopyright, AppPublisher,
    // AppPublisherURL, AppSupportPhone, AppSupportURL, AppUpdatesURL,
    // AppVersion, DefaultDirName, DefaultGroupName, BaseFilename,
    // UninstallFilesDir, UninstallDisplayName, UninstallDisplayIcon,
    // AppMutex, DefaultUserInfoName, DefaultUserInfoOrg,
    // DefaultUserInfoSerial, AppReadmeFile, AppContact, AppComments,
    // AppModifyPath, CreateUninstallRegKey, Uninstallable,
    // CloseApplicationsFilter.
    let s = r.wides(27)?;
    // LicenseText, InfoBeforeText, InfoAfterText, CompiledCodeText.
    for _ in 0..4 {
        r.ansi()?;
    }
    // Entry counts: languages, custom messages, permissions, types,
    // components, tasks, directories, files, file locations, icons, INI
    // entries, registry entries, install deletes, uninstall deletes, runs,
    // uninstall runs.
    let mut counts = [0usize; 16];
    for c in &mut counts {
        *c = r.count()?;
    }
    let [
        langs,
        msgs,
        perms,
        types,
        comps,
        tasks,
        dirs,
        files,
        nlocs,
        ..,
    ] = counts;
    r.versions()?;
    // BackColor, BackColor2, WizardImageBackColor; password SHA-1 and
    // salt; ExtraDiskSpaceRequired; SlicesPerDisk.
    r.skip(3 * 4 + 20 + 8 + 8 + 4)?;
    // UninstallLogMode, DirExistsWarning, PrivilegesRequired,
    // ShowLanguageDialog, LanguageDetectionMethod.
    r.skip(5)?;
    let compression = match r.u8()? {
        0 => Compression::Stored,
        1 => Compression::Zlib,
        2 => Compression::Bzip2,
        3 => Compression::Lzma,
        4 => Compression::Lzma2,
        c => return Err(Error::Corrupt(format!("compression method {c}"))),
    };
    // ArchitecturesAllowed, ArchitecturesInstallIn64BitMode,
    // DisableDirPage, DisableProgramGroupPage, UninstallDisplaySize,
    // Options (a set of 48 flags).
    r.skip(1 + 1 + 1 + 1 + 8 + 6)?;

    for _ in 0..langs {
        // Name, LanguageName, DialogFontName, TitleFontName,
        // WelcomeFontName, CopyrightFontName; Data, LicenseText,
        // InfoBeforeText, InfoAfterText; LanguageID, four font sizes,
        // RightToLeft.
        r.wides(6)?;
        for _ in 0..4 {
            r.ansi()?;
        }
        r.skip(4 + 4 * 4 + 1)?;
    }
    for _ in 0..msgs {
        // Name, Value, LangIndex.
        r.wides(2)?;
        r.skip(4)?;
    }
    for _ in 0..perms {
        r.ansi()?;
    }
    for _ in 0..types {
        // Name, Description, Languages, Check; versions; Options, Typ, Size.
        r.wides(4)?;
        r.versions()?;
        r.skip(1 + 1 + 8)?;
    }
    for _ in 0..comps {
        // Name, Description, Types, Languages, Check;
        // ExtraDiskSpaceRequired, Level, Used; versions; Options, Size.
        r.wides(5)?;
        r.skip(8 + 4 + 1)?;
        r.versions()?;
        r.skip(1 + 8)?;
    }
    for _ in 0..tasks {
        // Name, Description, GroupDescription, Components, Languages,
        // Check; Level, Used; versions; Options.
        r.wides(6)?;
        r.skip(4 + 1)?;
        r.versions()?;
        r.skip(1)?;
    }
    let mut dir_names = Vec::with_capacity(dirs);
    for _ in 0..dirs {
        // DirName, Components, Tasks, Languages, Check, AfterInstall,
        // BeforeInstall; Attribs; versions; PermissionsEntry; Options.
        let w = r.wides(7)?;
        dir_names.push(w[0].clone());
        r.skip(4)?;
        r.versions()?;
        r.skip(2 + 1)?;
    }
    let mut file_entries = Vec::with_capacity(files);
    for _ in 0..files {
        // SourceFilename, DestName, InstallFontName, StrongAssemblyName,
        // Components, Tasks, Languages, Check, AfterInstall,
        // BeforeInstall; versions; LocationEntry, Attribs, ExternalSize,
        // PermissionsEntry, Options (a set of 32 flags), FileType.
        let w = r.wides(10)?;
        r.versions()?;
        let loc = r.i32()?;
        r.skip(4 + 8 + 2 + 4)?;
        let file_type = r.u8()?;
        let location = match loc {
            -1 => None,
            l if l >= 0 && (l as usize) < nlocs => Some(l as u32),
            l => return Err(Error::Corrupt(format!("file location {l} of {nlocs}"))),
        };
        file_entries.push(FileEntry {
            dest: w[1].clone(),
            location,
            uninstaller: file_type == 1,
            components: w[4].clone(),
            languages: w[6].clone(),
        });
    }
    // Icons, INI and registry entries and the rest are not needed.

    const LOC_SIZE: usize = 74;
    if locations.len() != nlocs * LOC_SIZE {
        return Err(Error::Corrupt(format!(
            "{} bytes of file locations for {nlocs} entries",
            locations.len()
        )));
    }
    let mut r = Reader { d: locations, p: 0 };
    let mut locs = Vec::with_capacity(nlocs);
    for _ in 0..nlocs {
        let first_slice = r.u32()?;
        let last_slice = r.u32()?;
        let chunk_offset = r.u32()?;
        let offset = r.u64()?;
        let size = r.u64()?;
        let chunk_size = r.u64()?;
        let mut sha1 = [0u8; 20];
        sha1.copy_from_slice(r.bytes(20)?);
        let timestamp = r.u64()?;
        r.skip(8)?; // FileVersionMS, FileVersionLS
        let flags = r.u16()?;
        locs.push(Location {
            first_slice,
            last_slice,
            chunk_offset,
            offset,
            size,
            chunk_size,
            sha1,
            timestamp,
            flags,
        });
    }

    Ok(Setup {
        version: version.to_string(),
        app_name: s[0].clone(),
        app_version: s[9].clone(),
        compression,
        files: file_entries,
        dirs: dir_names,
        locations: locs,
    })
}
