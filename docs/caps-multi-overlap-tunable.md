# Caps Multi - Overlap Tunable 동작 명세

## 1. 목적

Caps 위치의 한 키에 다음 세 가지 역할을 부여한다.

- **Single Tap** → PC에서는 `LANG1`, Mac에서는 `Left Control + Space`
- **Single Hold** → `Left Control`
- **Tap → Second Hold** → `Layer 3` momentary 활성화

이 behavior는 단순한 Tap Dance와 Mod-Tap 중첩이 아니라, 입력 event를 잠시 보관하고 release 순서와 겹친 시간을 함께 판정하는 custom state machine이다. 결정된 출력은 다음과 같은 표준 ZMK child binding에 위임한다.

```dts
/* PC */
bindings = <&kp LANG1>, <&kp LCTRL>, <&mo 3>;

/* Mac */
bindings = <&kp LC(SPACE)>, <&kp LCTRL>, <&mo 3>;
```

ZMK Studio에서는 다음 두 behavior를 사용할 수 있다.

| Studio behavior | Keymap binding 예시 | Tap 출력 |
| --- | --- | --- |
| `Caps Multi - Overlap Tunable (PC)` | `&caps_multi_overlap_tunable 38` | `LANG1` |
| `Caps Multi - Overlap Tunable (Mac)` | `&caps_multi_overlap_tunable_mac 38` | `Left Control + Space` |

마지막 숫자는 해당 키에 적용할 overlap term `T`이며 단위는 ms이다.

## 2. 시간과 판정 기준

이 behavior에는 서로 다른 두 deadline이 있다.

- **Main deadline**: 첫 Caps press timestamp + `260ms`
- **Overlap deadline**: 첫 interrupt key press timestamp + `T`

`T`의 범위는 `5–100ms`이고 권장 시작값은 `38ms`이다. 두 deadline이 모두 존재할 때 먼저 도달하는 deadline이 현재 sequence를 resolve한다.

시간 비교 경계는 다음과 같다.

```text
elapsed < T   → release 순서로 Tap 또는 Ctrl 판정
elapsed >= T  → Ctrl 확정

elapsed < 260ms   → 같은 Caps sequence
elapsed >= 260ms  → 기존 sequence를 먼저 resolve
```

여기서 시간은 timer callback이 실제로 실행된 시각이 아니라 입력 event에 기록된 timestamp를 기준으로 판단한다. callback이 늦더라도 deadline 이후의 event를 처리하기 전에 deadline에 해당하는 state transition을 먼저 수행한다.

## 3. 핵심 동작 규칙

### Rule 1 — Single Tap

첫 Caps를 260ms 전에 release하고 그 뒤 두 번째 Caps나 다른 키 입력이 없다면 Tap 출력은 main deadline까지 pending 상태로 남는다.

```text
Caps DOWN
Caps UP
...
first Caps timestamp + 260ms
→ PC: LANG1
→ Mac: Left Control + Space
```

Caps release 순간에는 Tap을 바로 보내지 않는다. Tap → Second Hold 입력인지 판단할 시간이 필요하기 때문이다.

### Rule 2 — Tap Pending 중 다른 키 Press

Caps를 짧게 놓아 Tap이 pending인 상태에서 다른 키가 눌리면 다음 순서를 보장한다.

```text
pending Tap press/release
→ 다른 키 press
```

다른 키는 Control이나 Layer 3의 영향을 받지 않고 정상 layer에서 처리된다.

### Rule 3 — 다른 키 없이 Single Hold

Caps를 놓지 않고 main deadline까지 유지하면 `Left Control`이 활성화된다.

```text
0ms      Caps DOWN
260ms    LCTRL DOWN
...
         Caps UP
         LCTRL UP
```

### Rule 4 — 첫 Interrupt Key

Caps가 아직 눌린 상태에서 Caps가 아닌 다른 키가 처음 눌리면 그 키를 **first interrupt key**로 지정한다.

```text
Caps DOWN
A DOWN
→ A event를 즉시 host에 보내지 않고 capture
→ A press timestamp + T에 overlap deadline 설정
```

실제 resolve deadline은 main deadline과 overlap deadline 중 빠른 시각이다.

```text
resolve deadline = min(first Caps + 260ms, first interrupt + T)
```

결정이 끝날 때까지 들어오는 position event는 순서를 보존한 채 capture한다.

### Rule 5 — Overlap Deadline 전에 Caps가 먼저 Release

`T`가 지나기 전에 Caps release event가 먼저 들어오면 Tap으로 판정한다.

```text
Caps DOWN
A DOWN             → A DOWN capture
Caps UP, overlap < T

→ Tap press/release
→ captured A DOWN replay
```

이 규칙은 기계식 스위치의 접점이 아직 release되지 않은 짧은 순간에 다음 키를 빠르게 누른 경우, 의도하지 않은 Control chord가 되는 것을 줄인다.

### Rule 6 — First Interrupt Key가 먼저 Release

Caps보다 first interrupt key가 먼저 release되면 `T`가 지나지 않았더라도 즉시 Control로 확정한다.

```text
Caps DOWN
C DOWN             → C DOWN capture
C UP, overlap < T  → Ctrl 확정

→ LCTRL DOWN
→ captured C DOWN/UP replay
...
Caps UP
→ LCTRL UP
```

따라서 사용자가 `Ctrl+C`나 `Ctrl+V`를 입력하면서 문자 키를 먼저 놓는 습관도 정상적인 Control chord로 처리된다.

### Rule 7 — Overlap Term 도달

Caps와 first interrupt key가 함께 눌린 채 `T` 이상 유지되면 release 순서와 관계없이 Control로 확정한다.

```text
Caps DOWN
C DOWN
...
overlap = T
→ LCTRL DOWN
→ captured C DOWN replay
```

정확히 `T`인 시점은 Ctrl 쪽 경계이다. 즉 `overlap < T`일 때만 Caps-first release가 Tap을 선택할 수 있다.

### Rule 8 — Ctrl 확정은 되돌릴 수 없음

first interrupt release, overlap deadline 또는 main deadline으로 Control이 한 번 확정되면 이후 release 순서가 달라져도 Tap으로 바뀌지 않는다.

```text
Ctrl 확정
Caps UP
→ LCTRL UP
```

Control은 Caps가 release될 때까지 유지된다.

### Rule 9 — First Interrupt만 Release-Order 판정에 참여

결정 대기 중 여러 키가 들어올 수 있지만 release 순서를 결정하는 키는 first interrupt key뿐이다.

```text
Caps DOWN
A DOWN       → first interrupt
B DOWN       → capture만 수행
B UP         → 판정하지 않음
Caps UP < T  → Tap 확정 후 A/B event를 원래 순서로 replay
```

나중에 눌린 키의 release가 먼저 발생해도 그것만으로 Control을 확정하지 않는다.

### Rule 10 — Main Deadline이 먼저 도달

첫 interrupt가 늦게 시작해 overlap deadline보다 main deadline이 빠르면 260ms 규칙이 우선한다.

```text
0ms      Caps DOWN
240ms    A DOWN             → overlap deadline은 240ms + T
260ms    main deadline      → Ctrl 확정
         LCTRL DOWN
         captured A DOWN replay
```

따라서 Caps를 260ms 이상 유지한 입력은 overlap term과 관계없이 Hold이다.

### Rule 11 — Tap → Second Hold

첫 Caps를 Tap한 뒤 원래 main deadline 전에 같은 Caps를 다시 누르면 pending Tap을 폐기하고 Layer 3를 즉시 활성화한다.

```text
Caps DOWN
Caps UP             → Tap pending
Caps second DOWN    → pending Tap 폐기, Layer 3 ON
...
Caps second UP      → Layer 3 OFF
```

두 번째 Caps DOWN과 그에 대응하는 UP은 behavior 내부에서 소비한다. 따라서 Layer 3에 있는 Caps physical position의 binding은 실행되지 않으며, 두 번째 Caps 자체도 host keycode를 출력하지 않는다.

### Rule 12 — 260ms 이후의 Second Press

두 번째 Caps press timestamp가 원래 main deadline과 같거나 그보다 늦으면 이전 sequence의 second press가 아니다.

```text
0ms      Caps DOWN
50ms     Caps UP
260ms    pending Tap 출력
260ms+   Caps DOWN → 새로운 sequence의 first press
```

정확히 260ms에 들어온 second press도 이전 Tap을 먼저 resolve한 후 새 sequence를 시작한다.

### Rule 13 — Timer와 Capture 안전장치

Timer 취소만으로 correctness를 가정하지 않는다. 각 callback과 deferred capture는 현재 state와 sequence generation을 재검증하며, 이미 resolve되거나 취소된 sequence의 stale callback은 아무 출력도 만들지 않는다.

Capture buffer가 가득 차거나 capture를 시작할 수 없는 경우에는 입력을 잃지 않도록 안전하게 Control로 resolve하고 보관된 event를 replay한다.

## 4. 상태 머신

```text
                             ┌───────────────┐
                             │     IDLE      │
                             └───────┬───────┘
                                     │ Caps DOWN
                                     ▼
                         ┌────────────────────────┐
                         │       FIRST_DOWN       │
                         └──────┬─────────┬────────┘
                                │         │
             Caps UP < 260ms    │         │ first other key DOWN
                                │         │
                                ▼         ▼
                      ┌─────────────┐  ┌─────────────────────┐
                      │ TAP_PENDING │  │ INTERRUPT_UNDECIDED │
                      └───┬─────┬───┘  └─────┬─────┬─────┬───┘
                          │     │            │     │     │
                  timeout │     │ same Caps  │     │     │ main/overlap
               /other key │     │ DOWN       │     │     │ deadline
                          │     │            │     │     │
                          ▼     ▼            │     │     ▼
                         TAP  LAYER3_HELD     │     │  CTRL_HELD
                                │             │     │
                     second Caps UP     Caps UP     │ first interrupt UP
                                │          < T      │
                                ▼             │     │
                               IDLE           ▼     ▼
                                             TAP  CTRL_HELD

FIRST_DOWN -- main deadline 260ms ----------------> CTRL_HELD
CTRL_HELD  -- Caps UP ----------------------------> IDLE
```

`INTERRUPT_UNDECIDED`는 개념적인 상태명이다. 실제 구현에서는 `FIRST_DOWN` 상태를 유지하면서 capture ownership으로 interrupt 판정 중임을 나타낸다.

## 5. 시간축 예시

아래 예시에서 `T = 38ms`로 가정한다.

### A. PC에서 일반 한/영 전환

```text
0ms      Caps DOWN
50ms     Caps UP
260ms    LANG1 press/release
```

### B. Mac에서 일반 입력 소스 전환

```text
0ms      Caps DOWN
50ms     Caps UP
260ms    Left Control + Space press/release
```

### C. Caps가 먼저 올라오는 빠른 Roll

```text
0ms      Caps DOWN
50ms     A DOWN              → capture
70ms     Caps UP             → overlap 20ms < T
                              LANG1
                              A DOWN replay
120ms    A UP
```

결과:

```text
LANG1
→ A
```

A는 Control의 영향을 받지 않는다.

### D. 문자 키를 먼저 놓는 빠른 Ctrl+C

```text
0ms      Caps DOWN
40ms     C DOWN              → capture
60ms     C UP                → first interrupt release, Ctrl 확정
                              LCTRL DOWN
                              C DOWN/UP replay
90ms     Caps UP             → LCTRL UP
```

결과:

```text
Ctrl+C
```

### E. Overlap Term으로 Ctrl 확정

```text
0ms      Caps DOWN
40ms     C DOWN              → capture
78ms     overlap = 38ms      → Ctrl 확정
                              LCTRL DOWN
                              C DOWN replay
80ms     Caps UP             → LCTRL UP
100ms    C UP
```

78ms에 Ctrl이 확정됐으므로 이후 Caps가 먼저 올라와도 Tap으로 바뀌지 않는다.

### F. Caps만 오래 Hold

```text
0ms      Caps DOWN
260ms    LCTRL DOWN
500ms    Caps UP
         LCTRL UP
```

### G. Tap Pending 중 다른 키

```text
0ms      Caps DOWN
50ms     Caps UP             → Tap pending
120ms    A DOWN

처리 순서:
LANG1 press/release
→ A DOWN
```

Mac variant에서는 `LANG1` 대신 `Left Control + Space`를 먼저 전송한다.

### H. 늦은 Interrupt로 Main Deadline이 먼저 도달

```text
0ms      Caps DOWN
240ms    A DOWN              → capture, overlap deadline = 278ms
260ms    main deadline       → Ctrl 확정
                              LCTRL DOWN
                              A DOWN replay
```

### I. Tap → Second Hold

```text
0ms      Caps DOWN
50ms     Caps UP             → Tap pending
130ms    Caps second DOWN    → pending Tap 폐기, Layer 3 ON
180ms    A DOWN              → Layer 3의 A
250ms    A UP
300ms    Caps second UP      → Layer 3 OFF
```

첫 Tap의 언어 전환과 두 번째 Caps의 host keycode는 모두 출력되지 않는다.

## 6. 경계값

`T = 38ms`일 때의 대표적인 경계는 다음과 같다.

| 입력 | 결과 |
| --- | --- |
| Caps가 먼저 release, overlap `37ms` | Tap |
| Caps가 먼저 release, overlap `38ms` | Ctrl |
| First interrupt가 `38ms` 전에 먼저 release | Ctrl |
| Caps만 `259ms`에 release | Tap pending |
| Caps만 `260ms`에 release | Ctrl Hold sequence |
| Second Caps DOWN이 first press 후 `259ms` | Layer 3 |
| Second Caps DOWN이 first press 후 `260ms` | 이전 Tap 출력 후 새로운 first press |

## 7. Studio Parameter

`Overlap (ms)`는 전역 firmware 설정이 아니라 **각 key binding에 저장되는 parameter**이다.

```dts
&caps_multi_overlap_tunable 20
&caps_multi_overlap_tunable 50
```

따라서 같은 behavior를 여러 키에 배치하면서 서로 다른 `T`를 사용할 수 있다. ZMK Studio에 입력한 값은 persistent keymap에 저장되므로 일반 firmware flash 후에도 유지된다.

Parameter는 첫 Caps DOWN에서 해당 sequence용 값으로 복사된다. Studio에서 값을 변경하더라도 이미 진행 중인 sequence에는 영향을 주지 않고 다음 Caps press부터 적용된다.

- 허용 범위: `5–100ms`
- 권장 시작값: `38ms`
- 수동 keymap에서 범위를 벗어난 값: `38ms`로 fallback

작은 값은 Control을 더 빨리 확정하고, 큰 값은 Caps가 먼저 올라오는 빠른 roll을 Tap으로 인정할 여유를 늘린다.

## 8. 고정 Overlap Variant와의 관계

현재 compiled stock keymap은 다음 Tunable behavior를 `38ms` parameter로 사용한다.

- PC layer: `Caps Multi - Overlap Tunable (PC)`
- Mac layer: `Caps Multi - Overlap Tunable (Mac)`

고정 variant와 Tunable variant의 state machine은 같으며, overlap term의 출처만 다르다. 고정 variant도 비교용 Studio behavior로 남아 있고 그 값은 `38ms`이다.

```text
Fixed Overlap     → Devicetree의 overlap-term-ms = 38
Overlap Tunable   → key binding parameter로 T 입력
```

일반 firmware flash는 기존 Studio keymap을 보존하므로 기존의 고정 Caps binding이 새 stock binding으로 자동 교체되지는 않는다. 현재 Studio 설정을 유지하려면 Caps 위치에 Tunable behavior와 `38`을 직접 배치하면 된다. 새 compiled stock binding 전체를 불러오려는 경우에만 기존 runtime 변경을 지우는 **Restore Stock Settings**를 의도적으로 사용한다.

## 9. 한 문장 요약

**Caps를 한 번 탭하면 언어를 전환하고, 다른 키와 짧게 겹쳤을 때는 release 순서로 Tap과 Control을 구분하며, 설정한 시간 이상 겹치거나 상대 키를 먼저 놓으면 Control, 한 번 탭한 뒤 260ms 안에 다시 누르면 Layer 3가 되는 조절 가능한 three-way multi-role key이다.**
