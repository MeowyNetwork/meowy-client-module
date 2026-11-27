//! Who runs this build: the one place the launcher names the site it talks
//! to and the server it treats as its own. Kept in step with the pack's
//! `src/brand.h` and the installer's `Installer.ServerAddress`
//! (`tests/brand_test.js` checks the three agree).

/// The site: policy, updates and the server index, over HTTPS.
pub const HOST: &str = "meowynetwork.com";

/// The game servers this build treats as its own, as `ip:port`. Only these
/// ports: the same host also runs throwaway test servers.
pub const SERVERS: [&str; 1] = ["138.201.62.52:27015"];
