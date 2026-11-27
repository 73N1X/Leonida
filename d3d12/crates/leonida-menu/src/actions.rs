//! What the title's own scripts would have to do: god mode, wanted level,
//! vehicles, weapons, teleports, time and weather. A trainer for a RAGE
//! title does these by calling the script engine's natives; the runtime
//! cannot do that for this title yet (`docs/mod-menu.md` says what was
//! looked for in the executable and why it was not found). The menu's
//! items for them go through [`GameActions`], and the backend there is
//! today is [`NotYet`], which answers every one with the reason.

use crate::model::{Id, Value};

/// The features that need the title's scripts. Single player only: nothing
/// here touches other players, money or anything online.
pub trait GameActions: Send {
    /// `Err` with why the feature cannot work now.
    fn available(&self, id: Id) -> Result<(), String>;
    /// Its value now (a toggle's state, a number).
    fn value(&self, id: Id) -> Value;
    /// Does it: `Ok` with an optional line for the menu.
    fn apply(&mut self, id: Id, value: Value) -> Result<Option<String>, String>;
}

/// The reason given for every script feature today.
pub const NOT_YET: &str = "Not available yet: needs the title's script natives, which the runtime cannot call yet (docs/mod-menu.md).";

/// No script features.
pub struct NotYet;

impl GameActions for NotYet {
    fn available(&self, _: Id) -> Result<(), String> {
        Err(NOT_YET.into())
    }
    fn value(&self, _: Id) -> Value {
        Value::None
    }
    fn apply(&mut self, _: Id, _: Value) -> Result<Option<String>, String> {
        Err(NOT_YET.into())
    }
}
