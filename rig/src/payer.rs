//! Mint real ecash from a running micronuts-mint (FakeWallet backend)
//! and encode it as a hand-off token string (`payer` feature).

use core::fmt;
use std::sync::Arc;
use std::time::Duration;

use cdk::nuts::{CurrencyUnit, PaymentMethod};
use cdk::wallet::{SendOptions, Wallet};
use cdk::Amount;

#[derive(Debug)]
pub enum PayerError {
    Cdk(cdk::error::Error),
    Db(cdk::cdk_database::Error),
}

impl fmt::Display for PayerError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PayerError::Cdk(e) => write!(f, "cdk: {e}"),
            PayerError::Db(e) => write!(f, "wallet db: {e}"),
        }
    }
}

impl std::error::Error for PayerError {}

impl From<cdk::error::Error> for PayerError {
    fn from(e: cdk::error::Error) -> Self {
        PayerError::Cdk(e)
    }
}

impl From<cdk::cdk_database::Error> for PayerError {
    fn from(e: cdk::cdk_database::Error) -> Self {
        PayerError::Db(e)
    }
}

/// Mint `amount_sats` at `mint_url` and return the serialized token
/// string (cashuA/cashuB) for the atom to receive over NFC.
///
/// The mint must auto-settle quotes (the FakeWallet backend does); the
/// flow waits up to 20 s for the quote to become payable.
pub async fn mint_token(mint_url: &str, amount_sats: u64) -> Result<String, PayerError> {
    let localstore = Arc::new(cdk_sqlite::wallet::memory::empty().await?);
    let seed: [u8; 64] = rand::random();
    let wallet = Wallet::new(mint_url, CurrencyUnit::Sat, localstore, seed, None)?;

    let amount = Amount::from(amount_sats);
    let quote = wallet
        .mint_quote(PaymentMethod::BOLT11, Some(amount), None, None)
        .await?;
    let _received = wallet
        .wait_and_mint_quote(
            quote,
            Default::default(),
            Default::default(),
            Duration::from_secs(20),
        )
        .await?;

    let prepared = wallet.prepare_send(amount, SendOptions::default()).await?;
    let token = prepared.confirm(None).await?;
    Ok(token.to_string())
}

#[cfg(test)]
mod tests {
    #[cfg(feature = "payer")]
    #[tokio::test]
    #[ignore = "needs micronuts-audit-adapter on 127.0.0.1:3338"]
    async fn mints_a_token_from_the_running_mint() {
        let token = crate::payer::mint_token("http://127.0.0.1:3338", 21)
            .await
            .expect("mint flow");
        eprintln!("token ({} bytes): {}", token.len(), &token[..token.len().min(120)]);
        assert!(
            token.starts_with("cashuA") || token.starts_with("cashuB"),
            "unexpected token prefix: {:?}",
            &token[..token.len().min(12)]
        );
        assert!(token.len() > 100);
    }

    /// The e2e hands the token to the ACR1252U emulated tag, whose
    /// addressable image caps at 256 bytes — a 1-sat single-proof
    /// cashuB token must stay under that.
    #[cfg(feature = "payer")]
    #[tokio::test]
    #[ignore = "needs micronuts-audit-adapter on 127.0.0.1:3338"]
    async fn one_sat_token_fits_the_emulated_tag_area() {
        let token = crate::payer::mint_token("http://127.0.0.1:3338", 1)
            .await
            .expect("mint");
        eprintln!("1-sat token: {} bytes", token.len());
        eprintln!("TOKEN={token}");
        assert!(token.len() < 512, "unexpectedly large: {}", token.len());
    }
}
