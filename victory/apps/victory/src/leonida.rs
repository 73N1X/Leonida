//! Leonida: this runtime is Leonida's D3D12 path (the repository's `victory/`, built as `leonida.exe`).
//!
//! Every setting takes Leonida's name as well as VIctory's: `LEONIDA_X` stands for `VICTORY_X`. VIctory's name wins
//! where both are set, so an upstream script keeps meaning what it says; `LEONIDA_` names that are Leonida's own and
//! have no `VICTORY_` counterpart (`LEONIDA_ASPECT`, ...) are left for the code that reads them.

/// The `VICTORY_` settings to set for the environment `vars`: one per `LEONIDA_X` whose `VICTORY_X` is not set.
pub fn settings_names<I>(vars: I) -> Vec<(String, String)>
where
    I: IntoIterator<Item = (String, String)>,
{
    let vars: Vec<(String, String)> = vars.into_iter().collect();
    let set: std::collections::HashSet<&str> = vars.iter().map(|(k, _)| k.as_str()).collect();
    let mut out: Vec<(String, String)> = vars
        .iter()
        .filter_map(|(k, v)| {
            let rest = k.strip_prefix("LEONIDA_")?;
            if rest.is_empty() || OWN.contains(&rest) {
                return None;
            }
            let victory = format!("VICTORY_{rest}");
            (!set.contains(victory.as_str())).then(|| (victory, v.clone()))
        })
        .collect();
    out.sort();
    out
}

/// Leonida's own settings, read under their own names (not VIctory's).
const OWN: &[&str] = &["ASPECT", "PRESENT_SCALE", "WINEPREFIX", "VKD3D", "VICTORY"];

/// Applies [`settings_names`] to this process. Called first thing in `main`, before any thread starts.
pub fn apply_settings_names() {
    for (k, v) in settings_names(std::env::vars()) {
        // SAFETY: called at the start of `main`, before the runtime starts any thread, so nothing reads the
        // environment concurrently.
        unsafe { std::env::set_var(k, v) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vars(list: &[(&str, &str)]) -> Vec<(String, String)> {
        list.iter().map(|(k, v)| ((*k).to_owned(), (*v).to_owned())).collect()
    }

    #[test]
    fn leonida_names_stand_for_victory_ones() {
        let got = settings_names(vars(&[("LEONIDA_RENDER_SCALE", "0.5"), ("PATH", "x")]));
        assert_eq!(got, vars(&[("VICTORY_RENDER_SCALE", "0.5")]));
    }

    #[test]
    fn victory_name_wins() {
        let got = settings_names(vars(&[("LEONIDA_RELAY", "1"), ("VICTORY_RELAY", "2")]));
        assert!(got.is_empty());
    }

    #[test]
    fn leonida_own_settings_are_left_alone() {
        let got = settings_names(vars(&[("LEONIDA_ASPECT", "21:9"), ("LEONIDA_", "1")]));
        assert!(got.is_empty());
    }
}
