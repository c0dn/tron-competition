# TAVRN-BLE-V2.3 six-board proving layout

Observed by the user after exact-UID identification firmware displayed digits
`1` through `6`, without moving the boards:

```text
    1 (A)
    | short distance
2 (B) — 5 (E) — 6 (F) — 3 (C)  <------->  4 (D)
                 keyboard cluster          larger distance
```

| Number | Role | Probe UID | Canonical AdvA | SID16 | SID8 | Stable serial |
|---:|:---:|---|---|---:|---:|---|
| 1 | A | `9906360200052820cf57b9f988a30e16000000006e052820` | `18:42:de:52:4a:dd` | `0x4add` | `0xdd` | `/dev/serial/by-id/usb-Arm_BBC_micro:bit_CMSIS-DAP_9906360200052820cf57b9f988a30e16000000006e052820-if01` |
| 2 | B | `99063602000528205539bee7957c8dea000000006e052820` | `dc:4b:0a:06:03:f8` | `0x03f8` | `0xf8` | `/dev/serial/by-id/usb-Arm_BBC_micro:bit_CMSIS-DAP_99063602000528205539bee7957c8dea000000006e052820-if01` |
| 3 | C | `9906360200052820f9a4d9d29f9d7c0b000000006e052820` | `1e:33:a7:2f:8e:d8` | `0x8ed8` | `0xd8` | `/dev/serial/by-id/usb-Arm_BBC_micro:bit_CMSIS-DAP_9906360200052820f9a4d9d29f9d7c0b000000006e052820-if01` |
| 4 | D | `9906360200052820f4767fb3d81870df000000006e052820` | `51:56:ae:12:21:ca` | `0x21ca` | `0xca` | `/dev/serial/by-id/usb-Arm_BBC_micro:bit_CMSIS-DAP_9906360200052820f4767fb3d81870df000000006e052820-if01` |
| 5 | E | `99063602000528200b9c563b9bdebe86000000006e052820` | `be:65:0b:2c:96:d0` | `0x96d0` | `0xd0` | `/dev/serial/by-id/usb-Arm_BBC_micro:bit_CMSIS-DAP_99063602000528200b9c563b9bdebe86000000006e052820-if01` |
| 6 | F | `990636020005282033b7c2ad5952ee2e000000006e052820` | `56:a2:44:0e:9e:d6` | `0x9ed6` | `0xd6` | `/dev/serial/by-id/usb-Arm_BBC_micro:bit_CMSIS-DAP_990636020005282033b7c2ad5952ee2e000000006e052820-if01` |

All six full AdvAs, SID16 values, and fixed-k SID8 values are unique and
nonreserved. Primary comparison traffic is A→C with reciprocal logical A/C
direct-RX blocking; B, D, E, and F are available relay candidates. The
deterministic repair proof halts E/F and uses initial A-B-C plus alternate
B-D-C.
