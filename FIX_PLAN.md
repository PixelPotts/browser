# Hollywood Reporter Error Fix Plan

## Error Analysis & Fix Plan

These errors come from loading `hollywoodreporter.com` which uses Piano/Tinypass SDK, PMC plugins, consent management, and anti-adblock scripts. Below is each error categorized with root cause and fix.

---

### Error 1: `Load full version of piano SDK`
**Stack:** `tinypass.min.js` → `appendChild (native)` → `loadSDK (wordpress.js)`
**Root Cause:** The tinypass SDK's `loadSDK()` calls `appendChild` to inject a `<script>` tag. The loaded script runs initialization code that fails because our `tp` stub is an Array (line 3064), and the SDK expects an object with specific methods like `push()` that behave differently from Array.push.
**Fix:** Enhance the `tp` stub to be an object (not array) with a `push()` method that processes command tuples like `tp.push(["init", ...])`. Add `tp.initialize`, `tp.enableExternalCheckout`, and other methods the SDK expects. The interval-based patching (lines 3074-3086) should also patch `initialize`.

---

### Error 2: `cannot read property 'initialize' of undefined`
**Stack:** `wordpress.js` → `piano.es5.js`
**Root Cause:** After Piano SDK loads, `wordpress.js` tries to call `tp.piano.initialize()` but `tp.piano` is `undefined` at that moment — the SDK replaced `window.tp` with a new object, and the interval check hasn't run yet.
**Fix:** Make the interval patching also add `initialize: function(){}` to `tp.piano`. Better yet, use `Object.defineProperty` on `window` to intercept `tp` reassignment and auto-patch immediately rather than relying on polling.

---

### Error 3: `TypeError: not a function` (first one, no context)
**Root Cause:** Generic — something is called as a function but isn't one. Likely a callback from the Piano SDK flow. Will be resolved by fixing Errors 1 & 2.
**Fix:** Addressed by Piano stub improvements.

---

### Error 4: `typeof runTests = undefined` / `typeof Supports = undefined`
**Root Cause:** These are css3test.com debug messages, NOT related to hollywoodreporter. They're harmless warnings from a test suite that isn't loaded.
**Fix:** **No fix needed** — these are only relevant if browsing css3test.com.

---

### Error 5: `typeof window.onload = object`
**Root Cause:** Same as Error 4 — css3test debug output. Reports window.onload is type "object" (JS_NULL is typeof "object").
**Fix:** **No fix needed** — css3test-specific.

---

### Error 6: CSSKeyframesRule debug messages
**Root Cause:** Debug logging from css3test. Not errors.
**Fix:** **No fix needed**.

---

### Error 7: `cannot read property 'onLogin' of undefined` (DOMContentLoaded)
**Root Cause:** A DOMContentLoaded handler tries to access `something.onLogin` where `something` is undefined. Likely `window.pmcPiano.callbacks` gets wiped when PMC piano wordpress.js reinitializes. The wordpress.js plugin may overwrite `window.pmcPiano` entirely, losing our stub.
**Fix:** Protect `pmcPiano.callbacks` with the same polling approach used for `tp.piano`, or define it more defensively so it survives reassignment.

---

### Error 8: WhichBrowser stub warnings
**Root Cause:** The WhichBrowser timer fires, finds WhichBrowser undefined, defines it, logs confirmation. This is **expected behavior** — working as designed.
**Fix:** **No fix needed** — just status messages.

---

### Error 9: `cannot read property 'indexOf' of undefined` (setTimeout)
**Root Cause:** A setTimeout callback accesses a property that's a string (to call indexOf on it), but the property is undefined. Likely `navigator.userAgent` or some string property returning undefined, OR a DOM element property like `className` or `innerHTML` returning undefined.
**Fix:** Investigate — most likely a missing property on a DOM element or navigator object. Need to ensure common string properties return empty string rather than undefined.

---

### Error 10: `%cP%cM%cC Atlas MG...` + consent timeout
**Root Cause:** Two issues: (a) `console.log` doesn't handle `%c` format specifiers — it outputs them literally. (b) Consent management framework times out waiting for a response.
**Fix:**
- (a) Add `%c` format string stripping in `js_args_to_string()` — when the first argument contains `%c`, skip the corresponding style argument strings.
- (b) Enhance `__tcfapi` stub to also handle the `addEventListener` command which many CMPs use for event-driven consent. Add `__tcfapi('addEventListener', ...)` support.

---

### Error 11: `Failed to load website due to adblock`
**Root Cause:** Anti-adblock script tries to execute a "packing script" (likely `eval(function(p,a,c,k,e,d){...})` style packed JS). Since our browser doesn't block ads but may fail to load certain ad-related scripts, the anti-adblock detector incorrectly triggers.
**Fix:** Add a stub for common adblock detection methods. Many sites check for the existence of ad elements or use `document.write` — ensure `document.write`/`document.writeln` are implemented (currently missing entirely).

---

### Error 12: `TypeError: not a function` (setInterval)
**Root Cause:** An interval callback calls something that isn't a function. Likely a method on a stub object that returns undefined instead of a function, or a missing API method.
**Fix:** Will likely be resolved by Piano/PMC stub improvements. If not, we'll add additional method stubs after testing.

---

### Error 13: `TypeError: not a function` (setTimeout)
**Root Cause:** Same pattern as Error 12 — a timeout callback encounters a missing function.
**Fix:** Same approach — improve stubs, test, iterate.

---

## Implementation Order

### Phase 1: Core JS Engine Fixes
1. **Add `setTimeout`/`setInterval` string argument support** (js_engine.cpp:267-292)
   - When first argument is a string, eval it instead of rejecting silently
   - Many sites use `setTimeout("code()", 100)` pattern

2. **Add `console.log` %c format string handling** (js_engine.cpp:191-198)
   - Parse first arg for `%c` specifiers, skip corresponding style args
   - Also handle `%s`, `%d`, `%i`, `%f`, `%o` format specifiers

3. **Add `document.write`/`document.writeln`** (js_bindings.cpp)
   - Implement as no-ops that log warnings (calling document.write after parse is a no-op in real browsers too)

### Phase 2: Piano/PMC SDK Stub Improvements
4. **Rewrite `tp` stub** (js_engine.cpp:3062-3086)
   - Change from Array to Object with command-queue `push()` method
   - Add `initialize`, `enableExternalCheckout`, `setExternalCheckout` methods
   - Use `Object.defineProperty` to intercept `window.tp` reassignment

5. **Protect `pmcPiano.callbacks`** (js_engine.cpp:3092-3113)
   - Add interval-based protection like tp.piano
   - Ensure callbacks.onLogin always exists

6. **Enhance `__tcfapi` consent stub** (js_engine.cpp:3122)
   - Add `addEventListener` command support
   - Return proper consent granted response to prevent timeouts

### Phase 3: Testing & Iteration
7. **Build and test** on hollywoodreporter.com
8. **Fix any remaining `not a function` / `indexOf` errors** based on test output
9. **Verify no regressions** on html5test.com (must keep 425/425)

---

## Files to Modify
- `/mnt/1tb-ssd/random/browser/js_engine.cpp` — setTimeout string args, console %c, polyfill stubs
- `/mnt/1tb-ssd/random/browser/js_bindings.cpp` — document.write/writeln
