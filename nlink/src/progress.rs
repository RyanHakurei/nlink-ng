use std::sync::atomic::{AtomicPtr, AtomicU64, Ordering};

static REMAINING: AtomicU64 = AtomicU64::new(0);
static TOTAL: AtomicU64 = AtomicU64::new(0);

pub type ProgressHook = extern "C" fn(u64, u64);
static HOOK: AtomicPtr<()> = AtomicPtr::new(std::ptr::null_mut());

#[no_mangle]
pub extern "C" fn nlink_evo_set_progress_hook(hook: Option<ProgressHook>) {
  HOOK.store(
    hook.map(|f| f as *mut ()).unwrap_or(std::ptr::null_mut()),
    Ordering::Relaxed,
  );
}

fn note(remaining: u64, total: u64) {
  let ptr = HOOK.load(Ordering::Relaxed);
  if !ptr.is_null() {
    let hook: ProgressHook = unsafe { std::mem::transmute(ptr) };
    hook(remaining, total);
  }
}

pub fn reset(total: u64) {
  TOTAL.store(total, Ordering::Relaxed);
  REMAINING.store(total, Ordering::Relaxed);
  note(total, total);
}

pub fn set_remaining(remaining: u64) {
  let total = TOTAL.load(Ordering::Relaxed);
  if remaining > total {
    TOTAL.store(remaining, Ordering::Relaxed);
  }
  REMAINING.store(remaining, Ordering::Relaxed);
  note(remaining, TOTAL.load(Ordering::Relaxed));
}

pub fn update(remaining: u64, total: u64) {
  let total = total.max(remaining);
  TOTAL.store(total, Ordering::Relaxed);
  REMAINING.store(remaining, Ordering::Relaxed);
  note(remaining, total);
}

pub fn finish() {
  REMAINING.store(0, Ordering::Relaxed);
  note(0, TOTAL.load(Ordering::Relaxed));
}

pub fn get() -> (u64, u64) {
  let total = TOTAL.load(Ordering::Relaxed);
  let remaining = REMAINING.load(Ordering::Relaxed);
  (total.saturating_sub(remaining), total)
}
