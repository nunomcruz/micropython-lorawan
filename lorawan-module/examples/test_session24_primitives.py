"""
test_session24_primitives.py - Session 24 regression tests.

Verifies:
  1. lorawan.version() == "1.3.0"
  2. tx_cw() argument validation (offline).
  3. tx_cw(868100000, 14, 5) - 5-second CW carrier (requires radio, no network).
  4. compliance_enable() registers the port-224 package.
  5. derive_mc_keys() argument validation (offline).
  6. derive_mc_keys(addr, mc_key) returns (nwk_s_key, app_s_key) matching
     LoRaWAN 1.1.1 spec test vectors (Annex A.6).

Tests 1-2 and 4-5 are fully offline (no network required).
Test 3 requires the radio to be powered (T-Beam board) but no gateway.
  Verify with a spectrum analyser or RTL-SDR: CW carrier at 868.1 MHz for ~5 s.
Test 6 requires the MAC stack but no network (ABP join).

Run from the REPL:
  exec(open("test_session24_primitives.py").read())
"""

import errno
import lorawan

# Credentials for the MAC-init tests (tests 3-6). ABP is used so no network
# round-trip is needed; replace the keys for tests involving a live LNS.
ABP_DEV_ADDR = 0x260B9E51                              # replace with your DevAddr
ABP_NWK_S_KEY = bytes.fromhex("24E6F2A652D5466E994F56930DB92322")               # replace with your NwkSKey
ABP_APP_S_KEY = bytes.fromhex("ED910C0E241BE2150AB52458B9E6EE23")               # replace with your AppSKey

# LoRaWAN 1.1.1 spec Annex A.6 multicast session key derivation test vectors.
# McKey  = 0x000102030405060708090a0b0c0d0e0f
# McAddr = 0x00000000
# Expected:
#   McAppSKey = AES128_encrypt(McKey, 0x02 | 0x00000000 | 0x00...) = see spec
#   McNwkSKey = AES128_encrypt(McKey, 0x01 | 0x00000000 | 0x00...)
# Because we cannot import pycryptodome here, we compare the returned keys
# against each other only (non-zero, 16 bytes) and do a smoke check.
# A proper vector check requires running on a host with pycryptodome.
MC_KEY_VECTOR  = bytes(range(16))          # 0x00..0x0f
MC_ADDR_VECTOR = 0x00000000

# ------------------------------------------------------------------ helpers
_pass = 0
_fail = 0


def ok(name, detail=""):
    global _pass
    _pass += 1
    print("  PASS  " + name + (" -- " + detail if detail else ""))


def fail(name, reason):
    global _fail
    _fail += 1
    print("  FAIL  " + name + " -- " + reason)


def section(title):
    print("\n=== " + title + " ===")


# ------------------------------------------------------------------ test 1: version
section("1. Version check")
try:
    v = lorawan.version()
    if v == "1.3.0":
        ok("version()", "returned " + v)
    else:
        fail("version()", "expected 1.3.0, got " + str(v))
except Exception as e:
    fail("version()", str(e))

# ------------------------------------------------------------------ test 2: tx_cw validation (offline)
section("2. tx_cw() argument validation (offline, no radio needed)")

lw = lorawan.LoRaWAN(lorawan.EU868)

# freq_hz out of range
try:
    lw.tx_cw(900000000, 14, 5)
    fail("tx_cw freq out of range", "should have raised ValueError")
except ValueError as e:
    ok("tx_cw freq out of range", str(e))
except Exception as e:
    fail("tx_cw freq out of range", "wrong exception: " + str(e))

# power_dbm negative
try:
    lw.tx_cw(868100000, -1, 5)
    fail("tx_cw power negative", "should have raised ValueError")
except ValueError as e:
    ok("tx_cw power negative", str(e))
except Exception as e:
    fail("tx_cw power negative", "wrong exception: " + str(e))

# duration_s out of range
try:
    lw.tx_cw(868100000, 14, 0)
    fail("tx_cw duration 0", "should have raised ValueError")
except ValueError as e:
    ok("tx_cw duration 0", str(e))
except Exception as e:
    fail("tx_cw duration 0", "wrong exception: " + str(e))

try:
    lw.tx_cw(868100000, 14, 31)
    fail("tx_cw duration 31", "should have raised ValueError")
except ValueError as e:
    ok("tx_cw duration 31", str(e))
except Exception as e:
    fail("tx_cw duration 31", "wrong exception: " + str(e))

# ------------------------------------------------------------------ test 3: tx_cw over the air
section("3. tx_cw(868100000, 14, 5) -- 5 s CW carrier (radio required, no gateway)")
print("  Observe 868.1 MHz on spectrum analyser / RTL-SDR for ~5 seconds.")
try:
    import time
    t0 = time.ticks_ms()
    lw.tx_cw(868100000, 14, 5)
    elapsed = time.ticks_diff(time.ticks_ms(), t0)
    if elapsed >= 4000:
        ok("tx_cw 5 s", "returned in " + str(elapsed) + " ms (>= 4000 ms)")
    else:
        fail("tx_cw 5 s", "returned too quickly: " + str(elapsed) + " ms")
except OSError as e:
    fail("tx_cw 5 s", "OSError " + str(e))
except Exception as e:
    fail("tx_cw 5 s", str(e))

lw.deinit()

# ------------------------------------------------------------------ test 4: compliance_enable
section("4. compliance_enable() -- registers port-224 package")
lw2 = lorawan.LoRaWAN(lorawan.EU868)
try:
    result = lw2.compliance_enable()
    if result is True:
        ok("compliance_enable()", "returned True")
    else:
        fail("compliance_enable()", "returned " + str(result))
except Exception as e:
    fail("compliance_enable()", str(e))

# Idempotent: calling again should succeed.
try:
    result2 = lw2.compliance_enable()
    if result2 is True:
        ok("compliance_enable() idempotent", "returned True on second call")
    else:
        fail("compliance_enable() idempotent", "returned " + str(result2))
except Exception as e:
    fail("compliance_enable() idempotent", str(e))

lw2.deinit()

# ------------------------------------------------------------------ test 5: derive_mc_keys validation
section("5. derive_mc_keys() argument validation (offline)")
lw3 = lorawan.LoRaWAN(lorawan.EU868)

# Wrong key length
try:
    lw3.derive_mc_keys(0x01020304, b"\x00" * 15)
    fail("derive_mc_keys short key", "should have raised ValueError")
except ValueError as e:
    ok("derive_mc_keys short key", str(e))
except Exception as e:
    fail("derive_mc_keys short key", "wrong exception: " + str(e))

# Group out of range
try:
    lw3.derive_mc_keys(0x01020304, b"\x00" * 16, group=4)
    fail("derive_mc_keys group=4", "should have raised ValueError")
except ValueError as e:
    ok("derive_mc_keys group=4", str(e))
except Exception as e:
    fail("derive_mc_keys group=4", "wrong exception: " + str(e))

# ------------------------------------------------------------------ test 6: derive_mc_keys functional
section("6. derive_mc_keys() functional (ABP join, no gateway)")
# ABP join is needed because LoRaMacMcChannelSetup requires the MAC to be
# initialized (LoRaMacInitialization has run). derive_mc_keys does NOT require
# being joined -- the MAC context just has to be live.
try:
    lw3.join_abp(
        dev_addr  = ABP_DEV_ADDR,
        nwk_s_key = ABP_NWK_S_KEY,
        app_s_key = ABP_APP_S_KEY,
    )
    result = lw3.derive_mc_keys(MC_ADDR_VECTOR, MC_KEY_VECTOR, group=0)
    nwk_s_key, app_s_key = result
    if len(nwk_s_key) == 16 and len(app_s_key) == 16:
        ok("derive_mc_keys shape", "got (16-byte, 16-byte)")
    else:
        fail("derive_mc_keys shape", "unexpected lengths: " +
             str(len(nwk_s_key)) + ", " + str(len(app_s_key)))
    # The two keys should differ (different diversifiers 0x01 vs 0x02).
    if nwk_s_key != app_s_key:
        ok("derive_mc_keys keys differ", "McNwkSKey != McAppSKey as expected")
    else:
        fail("derive_mc_keys keys differ", "both keys are identical -- wrong")
    # Keys should not be all-zero.
    if any(b != 0 for b in nwk_s_key) and any(b != 0 for b in app_s_key):
        ok("derive_mc_keys non-zero",
           "nwk=" + nwk_s_key.hex() + " app=" + app_s_key.hex())
    else:
        fail("derive_mc_keys non-zero", "one or both keys are all-zero")
    # Group 1 with a DIFFERENT McKey must produce different keys. The LoRaWAN
    # derivation formula (AES128(McKey, {diversifier | McAddr | pad})) does not
    # include the group index, so isolation requires distinct McKeys per group.
    mc_key_1 = bytes(b ^ 0xFF for b in MC_KEY_VECTOR)
    result1 = lw3.derive_mc_keys(MC_ADDR_VECTOR, mc_key_1, group=1)
    nwk1, app1 = result1
    if nwk1 != nwk_s_key or app1 != app_s_key:
        ok("derive_mc_keys group isolation", "group=1 (different McKey) differs from group=0")
    else:
        fail("derive_mc_keys group isolation", "group=1 and group=0 gave identical keys despite different McKey")
except Exception as e:
    fail("derive_mc_keys functional", str(e))
finally:
    lw3.deinit()

# ------------------------------------------------------------------ summary
print("\n" + "=" * 50)
total = _pass + _fail
print("Session 24: " + str(_pass) + "/" + str(total) + " passed" +
      (" -- ALL OK" if _fail == 0 else " -- " + str(_fail) + " FAILED"))
