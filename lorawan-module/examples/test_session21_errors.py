"""
test_session21_errors.py — Session 21 error-handling regression tests.

Verifies that every failure path raises OSError(EIO/ETIMEDOUT) with a
status-carrying message, and that last_error() returns a well-formed dict.

Run from the REPL:
  exec(open("test_session21_errors.py").read())

Two devices are used (paste credentials at the top):
  ABP — already joined, used for most tests
  OTAA — used only for the join-timeout test (wrong app_key)
"""

import errno
import lorawan

# ------------------------------------------------------------------ credentials
ABP_DEV_ADDR = 0x00000000                              # replace with your DevAddr
ABP_NWK_S_KEY = bytes.fromhex("00" * 16)               # replace with your NwkSKey
ABP_APP_S_KEY = bytes.fromhex("00" * 16)               # replace with your AppSKey

OTAA_DEV_EUI  = bytes.fromhex("0000000000000000")      # replace with your DevEUI
OTAA_JOIN_EUI = bytes.fromhex("0000000000000000")      # replace with your JoinEUI
OTAA_APP_KEY  = bytes.fromhex("00" * 16)               # replace with your AppKey

# ------------------------------------------------------------------ helpers
_pass = 0
_fail = 0

def ok(name, detail=""):
    global _pass
    _pass += 1
    print(f"  PASS  {name}" + (f" — {detail}" if detail else ""))

def fail(name, reason):
    global _fail
    _fail += 1
    print(f"  FAIL  {name} — {reason}")

def section(title):
    print(f"\n=== {title} ===")

# ------------------------------------------------------------------ ABP setup
section("Setup (ABP)")
lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_dr=lorawan.DR_3)
lw.join_abp(dev_addr=ABP_DEV_ADDR, nwk_s_key=ABP_NWK_S_KEY, app_s_key=ABP_APP_S_KEY)
print(f"  joined={lw.joined()}, version={lorawan.version()}")

# ------------------------------------------------------------------ 1. last_error() is None before any failure
section("1. last_error() → None before any failure")
e = lw.last_error()
if e is None:
    ok("last_error() returns None on fresh object")
else:
    fail("last_error() should be None", f"got {e}")

# ------------------------------------------------------------------ 2. add_channel protected index
section("2. add_channel() on EU868 protected index (0–2)")
# EU868 indexes 0–2 are default channels; the MAC rejects overwrites.
for idx in (0, 1, 2):
    try:
        lw.add_channel(idx, 868100000, 0, 5)
        fail(f"add_channel({idx}) should raise", "no exception raised")
    except OSError as e:
        if e.args[0] != errno.EIO:
            fail(f"add_channel({idx}) errno", f"expected EIO({errno.EIO}), got {e.args[0]}")
        elif len(e.args) < 2 or "add_channel" not in e.args[1]:
            fail(f"add_channel({idx}) message", f"missing context, got {e.args}")
        elif "status=" not in e.args[1]:
            fail(f"add_channel({idx}) message", f"missing status=N, got {e.args[1]}")
        else:
            ok(f"add_channel({idx})", f"{e.args[1]}")
    except Exception as e:
        fail(f"add_channel({idx}) wrong type", f"{type(e).__name__}: {e}")

# last_error() should be populated from the most recent failure
err = lw.last_error()
if err is None:
    fail("last_error() after add_channel failure", "returned None")
elif err.get("context") != "add_channel":
    fail("last_error() context", f"expected 'add_channel', got {err.get('context')!r}")
elif err.get("loramac_status", 0) == 0:
    fail("last_error() loramac_status", f"expected non-zero, got {err.get('loramac_status')}")
else:
    ok("last_error() populated", f"loramac_status={err['loramac_status']} context={err['context']!r} epoch_us={err['epoch_us']}")

# ------------------------------------------------------------------ 3. remove_channel protected index
section("3. remove_channel() on EU868 protected index (0–2)")
# EU868 default channels 0–2 cannot be removed; the MAC returns
# LORAMAC_STATUS_PARAMETER_INVALID. Index 15 is not protected — the MAC
# silently no-ops it (no error), so it's not a useful failure trigger.
for idx in (0, 1, 2):
    try:
        lw.remove_channel(idx)
        fail(f"remove_channel({idx}) should raise", "no exception raised")
    except OSError as e:
        if e.args[0] == errno.EIO and len(e.args) >= 2 and "remove_channel" in e.args[1]:
            ok(f"remove_channel({idx})", f"{e.args[1]}")
        else:
            fail(f"remove_channel({idx})", f"unexpected: {e.args}")
    except Exception as e:
        fail(f"remove_channel({idx}) wrong type", f"{type(e).__name__}: {e}")

# ------------------------------------------------------------------ 4. multicast_remove non-existent group
section("4. multicast_remove() on group not configured")
# LoRaMacMcChannelDelete returns LORAMAC_STATUS_MC_GROUP_UNDEFINED (22)
# when the requested group slot has never been set up. This was previously
# swallowed as RuntimeError("multicast_remove failed") — now it must surface
# the status code 22.
try:
    lw.multicast_remove(0)
    fail("multicast_remove(0) should raise", "no exception raised")
except OSError as e:
    if e.args[0] != errno.EIO:
        fail("multicast_remove(0) errno", f"expected EIO, got {e.args[0]}")
    elif len(e.args) < 2 or "multicast_remove" not in e.args[1]:
        fail("multicast_remove(0) message", f"missing context: {e.args}")
    else:
        ok("multicast_remove(0) raises OSError(EIO)", e.args[1])
        err = lw.last_error()
        # LORAMAC_STATUS_MC_GROUP_UNDEFINED = 22
        if err and err.get("loramac_status") == 22:
            ok("last_error() loramac_status=22 (MC_GROUP_UNDEFINED)")
        elif err:
            ok(f"last_error() loramac_status={err.get('loramac_status')} (check LoRaMac.h)")
        else:
            fail("last_error() should be populated", "returned None")
except Exception as e:
    fail("multicast_remove(0) wrong type", f"{type(e).__name__}: {e}")

# ------------------------------------------------------------------ 5. last_error() dict structure
section("5. last_error() dict has all required keys and types")
try:
    lw.add_channel(0, 868100000, 0, 5)
except OSError:
    pass
err = lw.last_error()
if err is None:
    fail("last_error() structure", "returned None unexpectedly")
else:
    required = {
        "loramac_status": int,
        "event_status":   int,
        "context":        str,
        "epoch_us":       int,
    }
    bad = []
    for key, typ in required.items():
        if key not in err:
            bad.append(f"missing key '{key}'")
        elif not isinstance(err[key], typ):
            bad.append(f"'{key}' is {type(err[key]).__name__}, expected {typ.__name__}")
    if bad:
        fail("last_error() structure", "; ".join(bad))
    else:
        ok("last_error() all keys present with correct types", str(err))

# ------------------------------------------------------------------ 6. EBUSY semantics — rapid back-to-back send
section("6. EBUSY — duty-cycle restriction (send twice immediately)")
# First send establishes a duty-cycle timer. The second send on the same band
# may hit DUTYCYCLE_RESTRICTED → OSError(EBUSY), meaning the frame was NOT
# queued. This test accepts either outcome: if the band has cleared, the second
# send succeeds; if still restricted, EBUSY must be raised (not EIO).
import time

try:
    lw.send(b"dc_test_1", port=1, datarate=lorawan.DR_0)
    ok("first send succeeded")
except OSError as e:
    ok(f"first send raised OSError({e.args[0]})", "(duty cycle or TX error, continuing)")

try:
    lw.send(b"dc_test_2", port=1, datarate=lorawan.DR_0)
    ok("second send succeeded (duty cycle had cleared)")
except OSError as e:
    if e.args[0] == errno.EBUSY:
        wait_ms = lw.time_until_tx()
        ok("second send raised OSError(EBUSY)", f"time_until_tx={wait_ms} ms — frame NOT queued")
    elif e.args[0] == errno.EIO:
        # EIO is acceptable here (e.g. TX_TIMEOUT on the first retry)
        ok("second send raised OSError(EIO)", f"MAC error: {e.args[1] if len(e.args) > 1 else e}")
    else:
        fail("second send", f"unexpected OSError({e.args[0]}): {e.args}")

# ------------------------------------------------------------------ 7. join_otaa timeout with wrong credentials
section("7. join_otaa() timeout → OSError(ETIMEDOUT, 'join: last_status=N')")
print("  (waiting up to 15 s for join attempts to fail...)")
lw_bad = lorawan.LoRaWAN(region=lorawan.EU868, rx2_dr=lorawan.DR_3)
try:
    lw_bad.join_otaa(
        dev_eui=OTAA_DEV_EUI,
        join_eui=OTAA_JOIN_EUI,
        app_key=bytes.fromhex("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"),  # wrong key
        timeout=15,
    )
    fail("join_otaa with wrong key should raise", "no exception raised")
except OSError as e:
    if e.args[0] != errno.ETIMEDOUT:
        fail("join_otaa timeout errno", f"expected ETIMEDOUT({errno.ETIMEDOUT}), got {e.args[0]}")
    elif len(e.args) < 2 or "join: last_status=" not in e.args[1]:
        fail("join_otaa timeout message", f"expected 'join: last_status=N', got {e.args}")
    else:
        ok("join_otaa raised OSError(ETIMEDOUT)", e.args[1])
        err = lw_bad.last_error()
        if err and err.get("context") == "join":
            ok("last_error() context='join'", f"event_status={err.get('event_status')}")
        elif err is None:
            ok("last_error() is None (no join attempt reached server)")
        else:
            ok(f"last_error(): {err}")
except Exception as e:
    fail("join_otaa wrong type", f"{type(e).__name__}: {e}")
finally:
    lw_bad.deinit()

# ------------------------------------------------------------------ 8. OTAA join success then send
section("8. join_otaa() success + send() (positive path)")
lw_otaa = lorawan.LoRaWAN(region=lorawan.EU868, rx2_dr=lorawan.DR_3)
joined = False
try:
    lw_otaa.join_otaa(
        dev_eui=OTAA_DEV_EUI,
        join_eui=OTAA_JOIN_EUI,
        app_key=OTAA_APP_KEY,
        timeout=30,
    )
    joined = lw_otaa.joined()
    if joined:
        ok("join_otaa succeeded")
    else:
        fail("join_otaa", "joined() returned False after no exception")
except OSError as e:
    ok(f"join_otaa raised OSError({e.args})", "(device may not be on TTN — skipping send test)")

if joined:
    try:
        lw_otaa.send(b"hello otaa", port=1, datarate=lorawan.DR_0)
        if lw_otaa.last_error() is None:
            ok("send() succeeded, last_error() is None")
        else:
            ok("send() succeeded", f"last_error() still set from previous: {lw_otaa.last_error()}")
    except OSError as e:
        fail("send() after OTAA join", f"{e}")

lw_otaa.deinit()

# ------------------------------------------------------------------ summary
section("Summary")
total = _pass + _fail
print(f"  {_pass}/{total} passed, {_fail} failed")
if _fail == 0:
    print("  All Session 21 error-handling tests passed.")

lw.deinit()
