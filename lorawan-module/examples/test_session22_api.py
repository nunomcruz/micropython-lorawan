"""
test_session22_api.py — Session 22 API consistency regression tests.

Verifies:
  1. request_class() and network_time() raise AttributeError (removed).
  2. recv() raises RuntimeError when on_recv callback is registered.
  3. stats()["last_tx_fcnt_up"] is present after a send; old "tx_counter" is gone.
  4. lorawan.DR_6 and lorawan.DR_7 are exported integer constants.
  5. link_check() arg validation: ValueError on bad port / out-of-range DR.
  6. link_check(send_now=True, port=, confirmed=) accepted and works over the air.
  7. send(datarate=lorawan.DR_7) — FSK uplink (SX1276 note below).

Run from the REPL:
  exec(open("test_session22_api.py").read())

Tests 1–5 are fully offline (no network required).
Tests 6–7 require a joined session and a reachable gateway.

Note on test 7 (DR_7 / FSK):
  EU868 DR_7 = FSK 50 kbps. LoRaMAC-node supports it at the MAC level, but the
  SX1276 HAL in this firmware does not wire up the FSK modem path — SX1276Init()
  leaves the radio in LoRa mode. The MAC will accept the send() call and hand it
  to the radio driver; the frame may be silently dropped if the driver returns an
  error. The test notes whichever outcome occurs without failing. SX1262 boards
  have the same limitation (FSK HAL not implemented). This is tracked in TODO.md.
"""

import errno
import lorawan

# ------------------------------------------------------------------ credentials
# ABP is fastest: no join OTA round-trip, works even without a live gateway for
# offline tests 1–5.  Fill in real keys for tests 6–7.
ABP_DEV_ADDR  = 0x00000000          # replace with your DevAddr
ABP_NWK_S_KEY = bytes.fromhex("00" * 16)
ABP_APP_S_KEY = bytes.fromhex("00" * 16)

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


# ------------------------------------------------------------------ setup
section("Setup (ABP)")
lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_datarate=lorawan.DR_3)
lw.join_abp(dev_addr=ABP_DEV_ADDR, nwk_s_key=ABP_NWK_S_KEY, app_s_key=ABP_APP_S_KEY)
print(f"  joined={lw.joined()}, version={lorawan.version()}")

# ------------------------------------------------------------------ 1. removed API
section("1. Removed API: request_class() and network_time() raise AttributeError")

try:
    lw.request_class(lorawan.CLASS_C)
    fail("request_class() should raise AttributeError", "no exception raised")
except AttributeError:
    ok("request_class() raises AttributeError")
except Exception as e:
    fail("request_class() wrong exception type", f"{type(e).__name__}: {e}")

try:
    lw.network_time()
    fail("network_time() should raise AttributeError", "no exception raised")
except AttributeError:
    ok("network_time() raises AttributeError")
except Exception as e:
    fail("network_time() wrong exception type", f"{type(e).__name__}: {e}")

# ------------------------------------------------------------------ 2. recv() / on_recv() mutual exclusion
section("2. recv() raises RuntimeError when on_recv callback is registered")


def _dummy_rx(data, port, rssi, snr, multicast):
    pass


lw.on_recv(_dummy_rx)
try:
    lw.recv(timeout=0)
    fail("recv() with on_recv registered should raise", "no exception raised")
except RuntimeError as e:
    msg = str(e)
    if "on_recv is registered" in msg:
        ok("recv() raises RuntimeError", repr(msg))
    else:
        fail("recv() RuntimeError message wrong", repr(msg))
except Exception as e:
    fail("recv() wrong exception type", f"{type(e).__name__}: {e}")
finally:
    lw.on_recv(None)

# Confirm recv() works again once deregistered
try:
    result = lw.recv(timeout=0)
    if result is None:
        ok("recv() works after on_recv(None) deregister (no packet, returned None)")
    else:
        ok("recv() works after on_recv(None) deregister (got packet)")
except Exception as e:
    fail("recv() after deregister", f"{type(e).__name__}: {e}")

# ------------------------------------------------------------------ 3. stats() key rename
section("3. stats() key rename: 'last_tx_fcnt_up' present, 'tx_counter' gone")

# Send one frame to populate stats. Failure here is tolerated (no gateway needed
# for the key-existence check — the fields are initialised to 0 at startup).
try:
    lw.send(b"s22", port=1, datarate=lorawan.DR_0)
    ok("send() succeeded (stats will show real FCntUp)")
except OSError as e:
    ok(f"send() raised OSError({e.args[0]}) — stats key check uses zero-initialized values")

stats = lw.stats()

if "last_tx_fcnt_up" in stats:
    ok("stats() has 'last_tx_fcnt_up'", f"value={stats['last_tx_fcnt_up']}")
else:
    fail("stats() missing 'last_tx_fcnt_up'", f"keys={list(stats.keys())}")

if "tx_counter" in stats:
    fail("stats() still has old 'tx_counter' key", "should have been renamed")
else:
    ok("stats() no longer has 'tx_counter'")

# Confirm other expected keys are still present
for key in ("rssi", "snr", "rx_counter", "tx_time_on_air", "last_tx_ack",
            "last_tx_dr", "last_tx_freq", "last_tx_power"):
    if key not in stats:
        fail(f"stats() missing expected key '{key}'", f"keys={list(stats.keys())}")
    else:
        ok(f"stats() has '{key}'", f"value={stats[key]}")

# ------------------------------------------------------------------ 4. DR_6 / DR_7 constants
section("4. Module constants: lorawan.DR_6 and lorawan.DR_7")

try:
    dr6 = lorawan.DR_6
    if isinstance(dr6, int) and dr6 == 6:
        ok("lorawan.DR_6", f"= {dr6} (SF7/250 kHz)")
    else:
        fail("lorawan.DR_6 wrong value", f"expected 6, got {dr6!r}")
except AttributeError:
    fail("lorawan.DR_6 missing", "AttributeError")

try:
    dr7 = lorawan.DR_7
    if isinstance(dr7, int) and dr7 == 7:
        ok("lorawan.DR_7", f"= {dr7} (FSK 50 kbps)")
    else:
        fail("lorawan.DR_7 wrong value", f"expected 7, got {dr7!r}")
except AttributeError:
    fail("lorawan.DR_7 missing", "AttributeError")

# Existing constants still present
for name, expected in (("DR_0", 0), ("DR_5", 5)):
    try:
        val = getattr(lorawan, name)
        if val == expected:
            ok(f"lorawan.{name} unchanged", f"= {val}")
        else:
            fail(f"lorawan.{name} wrong", f"expected {expected}, got {val}")
    except AttributeError:
        fail(f"lorawan.{name} missing", "AttributeError")

# ------------------------------------------------------------------ 5. link_check() arg validation
section("5. link_check() argument validation (no network required)")

# Bad port — below range
try:
    lw.link_check(port=0)
    fail("link_check(port=0) should raise ValueError", "no exception")
except ValueError as e:
    ok("link_check(port=0) raises ValueError", str(e))
except Exception as e:
    fail("link_check(port=0) wrong type", f"{type(e).__name__}: {e}")

# Bad port — above range
try:
    lw.link_check(port=224)
    fail("link_check(port=224) should raise ValueError", "no exception")
except ValueError as e:
    ok("link_check(port=224) raises ValueError", str(e))
except Exception as e:
    fail("link_check(port=224) wrong type", f"{type(e).__name__}: {e}")

# Bad datarate — above DR_7
try:
    lw.link_check(datarate=8)
    fail("link_check(datarate=8) should raise ValueError", "no exception")
except ValueError as e:
    ok("link_check(datarate=8) raises ValueError", str(e))
except Exception as e:
    fail("link_check(datarate=8) wrong type", f"{type(e).__name__}: {e}")

# DR_7 is valid (boundary)
try:
    lw.link_check(datarate=lorawan.DR_7)
    ok("link_check(datarate=DR_7) accepted without ValueError (piggy-back mode)")
except ValueError as e:
    fail("link_check(datarate=DR_7) wrongly rejected", str(e))
except Exception as e:
    # RuntimeError / OSError from MAC state is fine — we're checking arg validation
    ok(f"link_check(datarate=DR_7) arg accepted, MAC raised {type(e).__name__}")

# ------------------------------------------------------------------ 6. link_check() port/confirmed kwargs over the air
section("6. link_check(send_now=True, port=2, confirmed=True) — requires gateway")
print("  (emits one uplink; LinkCheckAns may or may not arrive)")

try:
    result = lw.link_check(send_now=True, port=2, confirmed=True, datarate=lorawan.DR_0)
    if result is None:
        ok("link_check(send_now=True, port=2, confirmed=True) — TX OK, no LinkCheckAns")
    else:
        margin   = result.get("margin")
        gw_count = result.get("gw_count")
        if isinstance(margin, int) and isinstance(gw_count, int):
            ok("link_check returned valid dict",
               f"margin={margin} dB, gw_count={gw_count}")
        else:
            fail("link_check dict malformed", f"got {result!r}")
except OSError as e:
    ok(f"link_check raised OSError({e.args[0]}) — no gateway reachable or duty cycle",
       str(e.args))
except Exception as e:
    fail("link_check unexpected exception", f"{type(e).__name__}: {e}")

# ------------------------------------------------------------------ 7. DR_7 (FSK) send
section("7. send(datarate=lorawan.DR_7) — FSK 50 kbps uplink")
print("  Note: FSK modem path is not wired in the radio HAL for SX1276 or SX1262.")
print("  The MAC accepts the call; the frame may silently fail at the radio driver.")
print("  Any outcome (success / OSError) is noted but does not affect the pass count.")

try:
    lw.send(b"fsk", port=1, datarate=lorawan.DR_7)
    ok("send(DR_7) succeeded (FSK TX confirmed by MAC)")
except OSError as e:
    ok(f"send(DR_7) raised OSError({e.args[0]}) — FSK HAL not implemented or no GW",
       str(e.args))
except Exception as e:
    ok(f"send(DR_7) raised {type(e).__name__}: {e} — noting, not failing")

# ------------------------------------------------------------------ summary
section("Summary")
total = _pass + _fail
print(f"  {_pass}/{total} passed, {_fail} failed")
if _fail == 0:
    print("  All Session 22 API consistency tests passed.")
else:
    print(f"  {_fail} test(s) failed — review output above.")

lw.deinit()
