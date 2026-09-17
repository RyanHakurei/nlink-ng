use std::sync::atomic::{AtomicU64, Ordering};

static REMAINING: AtomicU64 = AtomicU64::new(0);
static TOTAL: AtomicU64 = AtomicU64::new(0);

pub fn reset(total: u64) {
  TOTAL.store(total, Ordering::Relaxed);
  REMAINING.store(total, Ordering::Relaxed);
}

pub fn set_remaining(remaining: u64) {
  let total = TOTAL.load(Ordering::Relaxed);
  if remaining > total {
    TOTAL.store(remaining, Ordering::Relaxed);
  }
  REMAINING.store(remaining, Ordering::Relaxed);
}

pub fn update(remaining: u64, total: u64) {
  TOTAL.store(total.max(remaining), Ordering::Relaxed);
  REMAINING.store(remaining, Ordering::Relaxed);
}

pub fn finish() {
  REMAINING.store(0, Ordering::Relaxed);
}

pub fn get() -> (u64, u64) {
  let total = TOTAL.load(Ordering::Relaxed);
  let remaining = REMAINING.load(Ordering::Relaxed);
  (total.saturating_sub(remaining), total)
}
