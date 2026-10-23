//! The project's releases on GitHub: the update check and the release's CIA.

use std::time::Duration;

/// The project's GitHub repository (owner/name): its releases page and the update check.
pub const REPOSITORY: &str = "Synaelle/OpenMW-3DS";

pub fn releases_page() -> String {
    format!("https://github.com/{REPOSITORY}/releases/latest")
}

/// The release's CIA, for FBI's "Scan QR code" (it downloads and installs it).
pub fn cia_url(version: &str) -> String {
    format!("https://github.com/{REPOSITORY}/releases/download/{version}/openmw3ds-{version}.cia")
}

/// The latest release's version on GitHub ("0.1.1", without the tag's "v"), or None when it can't be told (no
/// connection, no release yet). Blocks for a few seconds at most: call it from a thread. Testing:
/// OPENMW3DS_SETUP_REPOSITORY asks another repository.
pub fn latest_release_version() -> Option<String> {
    let repository = std::env::var("OPENMW3DS_SETUP_REPOSITORY").unwrap_or_else(|_| REPOSITORY.to_string());
    let agent: ureq::Agent =
        ureq::Agent::config_builder().timeout_global(Some(Duration::from_secs(5))).build().into();
    let mut response = agent
        .get(&format!("https://api.github.com/repos/{repository}/releases/latest"))
        .header("User-Agent", "openmw3ds-setup")
        .header("Accept", "application/vnd.github+json")
        .call()
        .ok()?;
    let body = response.body_mut().read_to_string().ok()?;
    // "tag_name": "v0.1.1"
    let at = body.find("\"tag_name\"")?;
    let rest = body[at + 10..].trim_start().strip_prefix(':')?.trim_start().strip_prefix('"')?;
    let tag = &rest[..rest.find('"')?];
    let version = tag.strip_prefix('v').unwrap_or(tag);
    parse(version).map(|_| version.to_string())
}

fn parse(version: &str) -> Option<[u32; 3]> {
    let mut parts = version.split('.').map(|p| {
        let digits: String = p.chars().take_while(char::is_ascii_digit).collect();
        digits.parse::<u32>().ok()
    });
    Some([parts.next()??, parts.next()??, parts.next()??])
}

/// Whether version a is older than b (both major.minor.micro).
pub fn older_than(a: &str, b: &str) -> bool {
    match (parse(a), parse(b)) {
        (Some(x), Some(y)) => x < y,
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compares_versions() {
        assert!(older_than("0.1.1", "0.2.0"));
        assert!(!older_than("0.2.0", "0.2.0"));
        assert!(older_than("0.1.1-dev", "0.1.2"));
    }
}
