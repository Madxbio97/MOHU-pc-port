# Keyboard and mouse controls

The launcher exposes the complete keyboard/mouse action catalog. Bindings are
saved in `%LOCALAPPDATA%\MedalOfHonorUndergroundPC\launcher.ini`.

The default layout follows a PC shooter scheme while retaining the retail
Configuration 1 D-pad controls:

| Action | Default | Retail input |
| --- | --- | --- |
| Move Forward | W | D-pad Up |
| Move Backward | S | D-pad Down |
| Move Left | A | L1 |
| Move Right | D | R1 |
| Lean Left | Q | L1 |
| Lean Right | E | R1 |
| Turn Left (keyboard fallback) | Left Arrow | D-pad Left |
| Turn Right (keyboard fallback) | Right Arrow | D-pad Right |
| Run | Left Shift | Native action; retail D-pad already uses full speed |
| Jump / Roll | Space | Triangle |
| Reload | R | Square |
| Aim | Mouse Right | R2 |
| Fire | Mouse Left | Cross |
| Crouch / Stealth | Left Ctrl | L3 |
| Action / Interact | F | Square |
| Target Lock | Tab | Layout-dependent |
| Quick Turn | Backspace | Native action |
| Quick Weapon Switch | Mouse Middle | Circle |
| Previous Weapon | [ | Circle |
| Next Weapon | ] | Circle |
| Weapon Menu Previous | Mouse Wheel Down | Circle |
| Weapon Menu Next | Mouse Wheel Up | Circle |
| Pause Menu | Escape | Start |
| Quick Weapon 1..10 | 1..0 | Circle/direct native slot |

Mouse capture is active whenever the game window has focus. Relative motion is
sent to RX/RY for layouts with camera-stick support. Configuration 1 also gets
D-pad turning and proportional LX/LY fallback, so the same mouse controls the
chase camera and the aimed reticle without switching input modes.

Physical bindings are stored under `[KeyboardMouse]`; guest button mappings
are stored as decimal 16-bit masks under `[RuntimeActions]`. Layout version 2
migrates the old A/D tank controls to WASD strafing, restores previously hidden
actions, and preserves user-customized bindings.
