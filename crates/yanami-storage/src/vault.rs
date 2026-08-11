use std::{collections::HashMap, sync::Mutex};

use secrecy::{ExposeSecret, SecretString};
use thiserror::Error;

#[derive(Debug, Error)]
pub enum CredentialError {
    #[error("credential vault failure: {0}")]
    Backend(String),
    #[error("credential vault lock is poisoned")]
    Poisoned,
}

pub trait CredentialVault: Send + Sync {
    fn set(&self, key: &str, value: &SecretString) -> Result<(), CredentialError>;
    fn get(&self, key: &str) -> Result<Option<SecretString>, CredentialError>;
    fn delete(&self, key: &str) -> Result<(), CredentialError>;
}

/// Production vault backed by Credential Manager, Keychain, or Secret Service.
pub struct SystemCredentialVault {
    service: String,
}

impl SystemCredentialVault {
    pub fn new(service: impl Into<String>) -> Self {
        Self {
            service: service.into(),
        }
    }

    fn entry(&self, key: &str) -> Result<keyring::Entry, CredentialError> {
        keyring::Entry::new(&self.service, key)
            .map_err(|error| CredentialError::Backend(error.to_string()))
    }
}

impl CredentialVault for SystemCredentialVault {
    fn set(&self, key: &str, value: &SecretString) -> Result<(), CredentialError> {
        self.entry(key)?
            .set_password(value.expose_secret())
            .map_err(|error| CredentialError::Backend(error.to_string()))
    }

    fn get(&self, key: &str) -> Result<Option<SecretString>, CredentialError> {
        match self.entry(key)?.get_password() {
            Ok(value) => Ok(Some(SecretString::from(value))),
            Err(keyring::Error::NoEntry) => Ok(None),
            Err(error) => Err(CredentialError::Backend(error.to_string())),
        }
    }

    fn delete(&self, key: &str) -> Result<(), CredentialError> {
        match self.entry(key)?.delete_credential() {
            Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
            Err(error) => Err(CredentialError::Backend(error.to_string())),
        }
    }
}

/// Session-only fallback used when a platform credential service is unavailable.
#[derive(Default)]
pub struct MemoryCredentialVault {
    values: Mutex<HashMap<String, SecretString>>,
}

impl CredentialVault for MemoryCredentialVault {
    fn set(&self, key: &str, value: &SecretString) -> Result<(), CredentialError> {
        self.values
            .lock()
            .map_err(|_| CredentialError::Poisoned)?
            .insert(key.to_owned(), value.clone());
        Ok(())
    }

    fn get(&self, key: &str) -> Result<Option<SecretString>, CredentialError> {
        Ok(self
            .values
            .lock()
            .map_err(|_| CredentialError::Poisoned)?
            .get(key)
            .cloned())
    }

    fn delete(&self, key: &str) -> Result<(), CredentialError> {
        self.values
            .lock()
            .map_err(|_| CredentialError::Poisoned)?
            .remove(key);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn memory_vault_round_trip_and_delete() {
        let vault = MemoryCredentialVault::default();
        vault.set("token", &SecretString::from("secret")).unwrap();
        assert_eq!(
            vault.get("token").unwrap().unwrap().expose_secret(),
            "secret"
        );
        vault.delete("token").unwrap();
        assert!(vault.get("token").unwrap().is_none());
    }
}
