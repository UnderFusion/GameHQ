# Windows acceptance for GameHQ capture

Plan item `lnov-38-bg-windows` covers issues [#38](https://github.com/UnderFusion/GameHQ/issues/38) and [#40](https://github.com/UnderFusion/GameHQ/issues/40). The baseline is published `v0.8.2-beta3` at `71a56d46f819ad3f2d63d68d1b1a2a1b76c3dc8f`. The candidate build in this worktree is `out/capture-38-40/GameHQ.exe`. Use the exact candidate commit and executable hash in the qualification receipt.

## Owner-controlled setup

1. Choose a Windows test window in which you can close the installed GameHQ instance and use two games and a controller. Keep each build's data in a separate portable profile. Put `portable.flag` beside the candidate executable and use the pinned Qt 6.11.3 MinGW runtime from the build environment.
2. Launch one framed game directly and one borderless game through Playnite. Choose scenes with different colors and distinct audio. Note the two game names, launch methods, Windows build, display scaling, HDR state, GPU and driver, audio output, controller and binding.
3. Use the same machine, display, games, scenes, audio device, controller, settings and measurement interval for Beta 3 and the candidate. Record the replay length, 1080p/30 FPS/14 Mbps settings, replay audio source, refresh rate and encoder reported in the GameHQ log.

## Capture and attribution

| Exercise | Expected evidence |
| --- | --- |
| Foreground capture, then automatic background capture | The foreground default follows the eligible game. With background enabled, switching to a browser retains the last eligible game. Focusing the second game starts its own replay. |
| Pin, select and unpin | Pin Game A while both are open. Focus Game B and save a screenshot and replay of A. Select B from the tray and repeat. Unpin and check the automatic target and tooltip. |
| Minimize, restore, close and relaunch | A minimized target pauses. After restore, verify fresh frames resume. Closing it clears its pin, and relaunching it requires a new pin. |
| Framed and borderless capture | Cover each game with another window or the taskbar. Move it partly off-screen and resize it. Verify screenshots and saved video show the game's client area. Repeat after moving between displays at different DPI if available. |
| SDR, HDR and replay disabled | Save a screenshot with replay off in both display modes. Verify the expected game, correct crop, and readable SDR colors for HDR capture. |
| Audio, overlay and controller | Exercise Game, Desktop and Off audio settings. For Game audio, play an identifiable sound in both games and verify only the selected game's audio is present. Check the overlay and Playnite pause menu, then use a controller tap and hold to confirm screenshot and replay feedback. |
| Gallery and old capture generation | Switch games immediately after saving a clip. Compare the clip's first frame and its preview to the game named in the gallery. Check first, middle and last decoded video frames and audio on both sides of the switch. Verify the new clip contains only the new game and the prior export retains its own title and preview. |

Save the actual PNG/JPEG, MP4, thumbnail, a tray capture and the matching log lines for each sampled case. Extract frames from the exported MP4 with a video decoder at independently selected timestamps. View them at full size to check the edges and the overlapping area. Compare the gallery preview with the first decoded frame, allowing for JPEG compression. Include one short replay and one replay after more than 60 seconds of background recording.

## Matched resource checks

- Run a common workload in both builds. Warm up for 60 seconds with the same game focused, capture for five minutes, then alternate focus between the two games 20 times at a fixed interval. Save at the same points in both runs. Repeat the pair three times when practical.
- On the candidate, repeat five minutes with the same game pinned behind the same non-game window. Repeat with automatic background capture. Collect GameHQ process private bytes, working set, handle count, CPU time deltas and GPU engine utilization at two-second intervals. Record GPU engine types and PID so that a game process or a reused PID is excluded. Windows Performance Monitor or Task Manager can confirm the GameHQ PID and the GPU engine names.
- Compare steady-state medians, peaks and the trend in private bytes and handles across switches. Include the time series and the maximum concurrent capture session and active encoder counts. Correlate the GameHQ log's start and stop lines, encoder reuse or fallback, and save-generation lines with the sample timestamps. Use a GPU trace if the engine counters and logs cannot distinguish the encoder lifetime.
- Confirm each generation stops before its replacement starts. Correlate validated video frames, audio and gallery identity with the matching save generation. Treat resource growth, duplicate sessions, video from an earlier target or a mismatched preview as failed acceptance.

Keep the owner-approved run's logs, decoded frames and measurements in `out/qualification/capture-38-40`. Attach the relevant results to `lnov-38-bg-windows` in Cockpit, together with any skipped case and its reason.
