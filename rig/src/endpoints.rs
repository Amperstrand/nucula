//! Bench endpoint resolution (issue #14): where the coordinator and mint
//! addresses come from, in precedence order:
//!
//! 1. **env** — `LABGRID_COORDINATOR` / `MINT_URL` (an operator's explicit
//!    override always wins; `LABGRID_COORDINATOR` mirrors the variable
//!    labgrid-client itself honors, `MINT_URL` is the existing payer
//!    convention);
//! 2. **registry** — the acquired labgrid place's comment, read live from
//!    the coordinator at acquire time (ADR-0005: conwrt-bench + labgrid
//!    places are the bench's single source of truth; the `ccid-nucula`
//!    place comment already carries its OTA endpoint this way). A
//!    `mint=<url>` token in the `nucula-rig` place comment makes bench
//!    migrations a comment edit, not a recompile;
//! 3. **fallback** — the compiled constants below (today's bench, kept as
//!    the last resort only).
//!
//! Pure logic only (no feature gating, no hardware): [`resolve_with`] takes
//! the registry comment as data so precedence is unit-testable; the IO
//! wrapper [`place_comment`] shells out to `labgrid-client show` (read-only)
//! and is what the live rig calls. Preflight reports which source each
//! endpoint resolved from.

use std::process::Command;

/// Path to labgrid-client (host tool, not a bench endpoint).
pub const LABGRID_CLIENT: &str = "/usr/local/bin/labgrid-client";

/// Compiled fallback: the labgrid coordinator (ai-legion:20408 today).
pub const FALLBACK_COORDINATOR: &str = "192.168.13.208:20408";
/// Compiled fallback: the relay mint's LAN bind (the URL tokens embed).
pub const FALLBACK_MINT: &str = "http://192.168.13.208:3338";
/// Env override for the coordinator (same name labgrid-client honors).
pub const ENV_COORDINATOR: &str = "LABGRID_COORDINATOR";
/// Env override for the mint (existing payer convention, see payer.rs).
pub const ENV_MINT: &str = "MINT_URL";

/// Where an endpoint value came from, cheapest authority last.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Source {
    Env(&'static str),
    Registry,
    Fallback,
}

impl Source {
    pub fn as_str(&self) -> &'static str {
        match self {
            Source::Env(name) => name,
            Source::Registry => "registry (place comment)",
            Source::Fallback => "compiled fallback",
        }
    }
}

/// The resolved bench endpoints plus the source of each.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct BenchEndpoints {
    pub coordinator: String,
    pub coordinator_source: Source,
    pub mint: String,
    pub mint_source: Source,
}

/// Read the place comment from the live coordinator (read-only `show`).
/// Returns `None` when the coordinator is unreachable, the client is
/// missing, or the place carries no comment — callers fall through to the
/// next authority.
pub fn place_comment(coordinator: &str, place: &str) -> Option<String> {
    let out = Command::new(LABGRID_CLIENT)
        .args(["-x", coordinator, "-p", place, "show"])
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    parse_place_comment(&String::from_utf8_lossy(&out.stdout))
}

/// Extract the `comment:` value from `labgrid-client show` text output.
///
/// The shape (captured from the live coordinator): keys are indented two
/// spaces (`  comment: …`, `  matches: …`); a comment may continue on
/// more-indented lines. Everything after `comment:` up to the next
/// two-space key is the comment.
pub fn parse_place_comment(show: &str) -> Option<String> {
    let mut lines = show.lines().skip_while(|l| !l.starts_with("  comment:"));
    let first = lines.next()?;
    let mut comment = first.trim_start_matches("  comment:").trim().to_string();
    for l in lines {
        // deeper indentation than a key line = continuation of the comment
        if l.starts_with("    ") && !l.starts_with("      comment") {
            comment.push(' ');
            comment.push_str(l.trim());
        } else {
            break;
        }
    }
    if comment.is_empty() {
        None
    } else {
        Some(comment)
    }
}

/// Pull a `mint=<url>` token out of a place comment. Only absolute
/// http(s) URLs count — anything else is prose (e.g. the OTA endpoint
/// sentence on `ccid-nucula`), not a directive.
pub fn mint_from_comment(comment: &str) -> Option<&str> {
    comment.split_whitespace().find_map(|tok| {
        let v = tok.strip_prefix("mint=")?;
        if v.starts_with("http://") || v.starts_with("https://") {
            Some(v)
        } else {
            None
        }
    })
}

/// Resolve endpoints with explicit env values (deterministic, testable).
pub fn resolve_with_env(
    registry_comment: Option<&str>,
    env_coordinator: Option<&str>,
    env_mint: Option<&str>,
) -> BenchEndpoints {
    let (coordinator, coordinator_source) = match env_coordinator.filter(|v| !v.is_empty()) {
        Some(v) => (v.to_string(), Source::Env(ENV_COORDINATOR)),
        None => (FALLBACK_COORDINATOR.to_string(), Source::Fallback),
    };
    // The coordinator address is the bootstrap endpoint — the registry is
    // reached THROUGH it, so it cannot come from the registry: env or
    // fallback only.

    let (mint, mint_source) = if let Some(v) = env_mint.filter(|v| !v.is_empty()) {
        (v.to_string(), Source::Env(ENV_MINT))
    } else if let Some(url) = registry_comment.and_then(mint_from_comment) {
        (url.to_string(), Source::Registry)
    } else {
        (FALLBACK_MINT.to_string(), Source::Fallback)
    };

    BenchEndpoints {
        coordinator,
        coordinator_source,
        mint,
        mint_source,
    }
}

/// Resolve endpoints against the live registry (read-only) + real env.
pub fn resolve(registry_comment: Option<&str>) -> BenchEndpoints {
    resolve_with_env(
        registry_comment,
        std::env::var(ENV_COORDINATOR).ok().as_deref(),
        std::env::var(ENV_MINT).ok().as_deref(),
    )
}
