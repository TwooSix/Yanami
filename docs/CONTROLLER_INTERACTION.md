# Controller interaction

Yanami treats controllers as an input modality, not as a second set of page-specific shortcuts. Device backends translate physical inputs into semantic actions; QML consumes those actions and the same focus graph used by keyboard navigation. Pointer input remains independent and the most recent meaningful input wins without moving focus merely because a device connected.

## Support levels

| Device family | Default profile | Support level | Release claim |
| --- | --- | --- | --- |
| Xbox / XInput-compatible | Xbox | Implemented; hardware pending | The software path is release-ready, but the Windows Xbox hardware acceptance matrix is still required before release. |
| PlayStation DualShock / DualSense | PlayStation | Experimental | SDL mapping, synthetic contracts, and process-local virtual-device raw-path automation are covered; real hardware is still untested. |
| Nintendo Switch Pro / Joy-Con pair | Nintendo | Experimental | SDL mapping, synthetic contracts, and process-local virtual-device raw-path automation are covered; real hardware is still untested. |
| TV remote / media keyboard HID | Remote | Experimental | Standard navigation/media keys and Windows HID classification are supported; vendor learning requires a real remote profile. |
| Unknown SDL gamepad | Generic | Generic | Navigation is best effort and the Settings diagnostics page exposes the detected identity. |

An experimental label is intentionally visible in Settings. Xbox remains hardware-pending until its physical acceptance matrix is complete. Feedback obtained on Xbox may then be applied to shared semantics and focus behavior for every family, but it must not upgrade another family to verified without hardware evidence.

## Runtime architecture

```text
SDL3 gamepad ─┐
              ├─ device profile ─ neutral gate / repeat ─ semantic actions ─ QML focus and page policies
XInput fallback┘

Qt key events ─ Windows Raw Input identity ─ remote profile ────────────────┘
mouse / keyboard ─────────────────────── modality arbitration ──────────────┘
```

- SDL3 is the primary gamepad backend. XInput is an exclusive Windows fallback when SDL3 cannot initialize; they never read the same pad concurrently.
- Connection, disconnection, backend choice, active-device changes, and errors are logged. Raw axes are deliberately not logged.
- A newly connected or reconnected device must return to neutral before it can produce actions. This prevents held buttons or sticks from acting during hot-plug.
- A transient SDL or Raw Input enumeration failure retains the last successful device snapshot; only a successful scan may publish disconnects.
- Connecting a device does not steal modality or focus. Only a meaningful action makes it the active device. Mouse, keyboard, controller, and remote can therefore switch without a mode dialog.
- Windows Raw Input supplies HID identity for remote classification. Qt key events remain the single action source, which avoids double dispatch.
- An uncorrelated dedicated media key uses an ephemeral Remote action descriptor for prompts and diagnostics, but is not listed as connected hardware.
- Device descriptors exposed to QML contain a runtime identity, display name, family, support tier, backend, and connection state. Pages never branch on vendor IDs; reconnecting hardware may receive a new runtime identity.
- A known remote profile or a correlated HID consumer-control collection identifies the Remote modality. An unknown arrow-only remote is intentionally treated as Keyboard until its VID/PID profile is added, avoiding false classification of ordinary keyboards.
- Settings diagnostics are passive by default. `Start test` acquires an exclusive, owner-bound capture; actions then update diagnostics without navigating or activating the application. Back exits the test, and leaving the section or page releases capture automatically.

## Semantic actions

The native boundary exposes navigation, activation, back, context, application menu, search, paging, scrolling, playback, seek, volume, and previous/next-item actions. Only `Navigate*`, `Activate`, `Back`, and `Context` synthesize compatible Qt keys. All other actions are semantic-only so a single press cannot trigger both a shortcut and a page handler.

Context priority is:

1. top popup or modal;
2. focused control;
3. current page or player mode;
4. application navigation and window commands.

The focus navigator uses explicit relationships where product meaning matters and geometry as a fallback. It ignores hidden, disabled, zero-sized, or out-of-scope targets, reveals virtualized delegates before focusing them, and restores semantic focus after a popup or page round trip.

## Default physical mapping

| Semantic action | Xbox | PlayStation | Nintendo | TV remote |
| --- | --- | --- | --- | --- |
| Navigate | D-pad / left stick | D-pad / left stick | D-pad / left stick | arrows |
| Activate | A | Cross | A (east) | OK / Enter |
| Back | B | Circle | B (south) | Back / Escape |
| Context | X | Square | Y | Options / context key |
| Search / player fullscreen | Y | Triangle | X | Search |
| Application/player menu | Menu | Options | Plus | Menu |
| Previous/next page | LB / RB | L1 / R1 | L / R | channel/page keys |
| Scroll | right stick | right stick | right stick | page keys |
| Seek | LT / RT | L2 / R2 | ZL / ZR | rewind / fast-forward |
| Playback/media | Activate in ambient player mode | same | same | play/pause, seek, volume keys |

Inside the player, the default state is an ambient mode: Activate toggles play/pause, left/right seek, and up/down change volume while showing a transient volume meter. Search (Xbox Y, PlayStation Triangle, Nintendo X, or a remote Search key) toggles fullscreen in either player mode. Menu enters control-bar focus mode; while focused, navigation moves among controls and Activate operates the selected control. When Skip Intro is offered, it remains available for the complete server-authored intro marker and is reachable by moving up from the timeline. Back closes the innermost menu, leaves control-bar mode, and only then leaves playback.

## Explicit exclusions

Every actionable in-application control is expected to be reachable without a pointer, except:

- entering or editing arbitrary text, including IME composition and secrets;
- completing work in an operating-system or external-application window after Yanami opens it;
- arbitrary frameless-window dragging or edge resizing;
- vendor-only controller features such as touchpads, adaptive triggers, speaker, gyro, NFC, or HD rumble;
- unknown proprietary remote keys until a VID/PID profile is added.

Text fields can still be focused, submitted, or left with a controller; Context clears the focused field and an inline prompt exposes that shortcut. Window minimize, maximize/restore, and close remain reachable through application controls.

## Acceptance gates

Automated acceptance must prove backend selection, device classification, profile mapping, neutral-on-connect, repeat timing, active-device arbitration, key/semantic de-duplication, and focus/popup contracts. A clean Windows release build, the complete CTest suite, QML tests, runtime smoke test, dependency closure, and diff hygiene must pass.

The native suite attaches virtual Xbox, PlayStation, and Nintendo gamepads inside the test process and drives the production SDL polling path for each profile. This closes every supported gamepad family's raw SDL state-to-semantic-action loop without a kernel driver, including its confirm/back face-button convention. SDL virtual devices are process-local, so these tests complement rather than replace Windows PnP, XInput fallback, unplug/replug, and physical-button acceptance.

Xbox release acceptance additionally requires real-hardware runs for cold start, hot-plug, unplug/replug while a button is held, mouse/keyboard/controller switching, every top-level page, search without text entry, menus and dialogs, the two player modes, playback completion/retry, Settings diagnostics, and window commands. No focus trap, double activation, accidental destructive default, or unintended action on connection is acceptable.

PlayStation and Nintendo acceptance includes profile, synthetic-contract, and process-local SDL virtual-device raw-path tests. Because corresponding physical hardware has not been tested, their result must remain `experimental`, never `passed`, in release evidence. TV remote acceptance remains limited to profile and synthetic tests until corresponding hardware is available and must likewise remain `experimental`.
