/** @file
  perseus (Xiaomi MIX 3) touch probe: bring the ST FTS up from UEFI and talk FTM5 to it.

  The touch controller of this phone belongs to the firmware's world: the I/O rail is a TLMM pin,
  the bus is QUP SE14 and the PMIC must not be poked (doing it from XNU resets the machine, and
  this firmware has no other user of the VREG protocol). So the chip is driven here -- rail, reset
  and a bit-banged I2C on GPIO33/34 -- and XNU only ever sees the events this code produces.

  FTM5 (as in the kernel driver): a command is written *without a stop*, then the answer is read
  with a repeated start. A register-style read returns 0xFF and wedges the chip.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/QcomI2c.h>
#include <Protocol/QcomTlmm.h>
#include <Protocol/QcomPmicVreg.h>

#define PIN_INT         31
#define PIN_RESET       32
#define PIN_SDA         33
#define PIN_SCL         34
#define PIN_VDDIO       79
#define I2C_ADDR_ST     0x49
#define REG_CHIP_ID     0xA3

#define FTM5_READ_EVENT   0x86
#define FTM5_HW_REG       0xFA
#define FTM5_FRAMEBUFFER  0xA6
#define EVT_CONTROLLER_READY 0x03

#define BB_DELAY_US     10

STATIC EFI_QCOM_TLMM_PROTOCOL      *mTlmm;
STATIC EFI_QCOM_I2C_PROTOCOL       *mI2c;
STATIC EFI_QCOM_PMIC_VREG_PROTOCOL *mVreg;

/* ---- TLMM ---------------------------------------------------------------------------------- */

STATIC
UINT32
PinLevel (
  IN UINT32 Pin
  )
{
  UINT32 value = 0xFFFFFFFF;
  UINT32 cfg   = EFI_GPIO_CFG (Pin, 0, GPIO_INPUT, GPIO_PULL_UP, GPIO_2MA);

  if (EFI_ERROR (mTlmm->GpioIn (cfg, &value))) {
    return 0xFFFFFFFF;
  }
  return value;
}

STATIC
EFI_STATUS
Gpio (
  IN UINT32 Pin,
  IN UINT32 Direction,
  IN UINT32 Pull,
  IN UINT32 Level
  )
{
  EFI_STATUS Status;
  UINT32     cfg = EFI_GPIO_CFG (Pin, 0, Direction, Pull, GPIO_2MA);

  Status = mTlmm->ConfigGpio (cfg, TLMM_GPIO_ENABLE);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return mTlmm->GpioOut (cfg, Level);
}

/* ---- bit-banged I2C on GPIO33/34 ------------------------------------------------------------ */

STATIC VOID BbDelay (VOID) { gBS->Stall (BB_DELAY_US); }

STATIC
VOID
SdaLow (
  VOID
  )
{
  mTlmm->ConfigGpio (EFI_GPIO_CFG (PIN_SDA, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA), TLMM_GPIO_ENABLE);
  mTlmm->GpioOut (EFI_GPIO_CFG (PIN_SDA, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA), 0);
}

STATIC
VOID
SdaRelease (
  VOID
  )
{
  mTlmm->ConfigGpio (EFI_GPIO_CFG (PIN_SDA, 0, GPIO_INPUT, GPIO_PULL_UP, GPIO_2MA), TLMM_GPIO_ENABLE);
}

STATIC
UINT32
SdaRead (
  VOID
  )
{
  UINT32 v = 1;

  mTlmm->GpioIn (EFI_GPIO_CFG (PIN_SDA, 0, GPIO_INPUT, GPIO_PULL_UP, GPIO_2MA), &v);
  return v & 1;
}

STATIC
VOID
SclLow (
  VOID
  )
{
  mTlmm->ConfigGpio (EFI_GPIO_CFG (PIN_SCL, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA), TLMM_GPIO_ENABLE);
  mTlmm->GpioOut (EFI_GPIO_CFG (PIN_SCL, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA), 0);
}

STATIC
VOID
SclRelease (
  VOID
  )
{
  mTlmm->ConfigGpio (EFI_GPIO_CFG (PIN_SCL, 0, GPIO_INPUT, GPIO_PULL_UP, GPIO_2MA), TLMM_GPIO_ENABLE);
}

STATIC
VOID
BbStart (
  VOID
  )
{
  SdaRelease (); SclRelease (); BbDelay ();
  SdaLow ();     BbDelay ();
  SclLow ();     BbDelay ();
}

STATIC
VOID
BbStop (
  VOID
  )
{
  SdaLow ();     BbDelay ();
  SclRelease (); BbDelay ();
  SdaRelease (); BbDelay ();
}

STATIC
UINT32
BbWriteByte (
  IN UINT8 Byte
  )
{
  UINT32 i, ack;

  for (i = 0; i < 8; i++) {
    if (Byte & 0x80) {
      SdaRelease ();
    } else {
      SdaLow ();
    }
    Byte <<= 1;
    BbDelay ();
    SclRelease (); BbDelay ();
    SclLow ();     BbDelay ();
  }
  SdaRelease (); BbDelay ();
  SclRelease (); BbDelay ();
  ack = !SdaRead ();
  SclLow ();     BbDelay ();
  return ack;
}

STATIC
UINT8
BbReadByte (
  IN UINT32 Ack
  )
{
  UINT32 i;
  UINT8  value = 0;

  SdaRelease ();
  for (i = 0; i < 8; i++) {
    BbDelay ();
    SclRelease (); BbDelay ();
    value = (UINT8)((value << 1) | (UINT32)SdaRead ());
    SclLow ();     BbDelay ();
  }
  if (Ack) {
    SdaLow ();
  } else {
    SdaRelease ();
  }
  BbDelay ();
  SclRelease (); BbDelay ();
  SclLow ();     BbDelay ();
  SdaRelease ();
  return value;
}


/* write, then a stop */
STATIC
EFI_STATUS
BbWrite (
  IN UINT16      Address,
  IN CONST UINT8 *Buffer,
  IN UINT32      Length
  )
{
  UINT32 i;

  BbStart ();
  if (!BbWriteByte ((UINT8)(Address << 1))) {
    BbStop ();
    return EFI_DEVICE_ERROR;
  }
  for (i = 0; i < Length; i++) {
    if (!BbWriteByte (Buffer[i])) {
      BbStop ();
      return EFI_DEVICE_ERROR;
    }
  }
  BbStop ();
  return EFI_SUCCESS;
}
/* write, no stop: the read that follows starts again (the kernel's bb_write(stretch=true)) */
STATIC
EFI_STATUS
BbWriteStretch (
  IN UINT16      Address,
  IN CONST UINT8 *Buffer,
  IN UINT32      Length
  )
{
  UINT32 i;

  BbStart ();
  if (!BbWriteByte ((UINT8)(Address << 1))) {
    return EFI_DEVICE_ERROR;
  }
  for (i = 0; i < Length; i++) {
    if (!BbWriteByte (Buffer[i])) {
      return EFI_DEVICE_ERROR;
    }
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
BbRead (
  IN UINT16 Address,
  OUT UINT8 *Buffer,
  IN UINT32 Length
  )
{
  UINT32 i;

  BbStart ();
  if (!BbWriteByte ((UINT8)((Address << 1) | 1))) {
    BbStop ();
    return EFI_DEVICE_ERROR;
  }
  for (i = 0; i < Length; i++) {
    Buffer[i] = BbReadByte (i + 1 < Length);
  }
  BbStop ();
  return EFI_SUCCESS;
}

/* free a chip that holds the bus: let go, nine clocks, stop */
STATIC
VOID
BbRecover (
  VOID
  )
{
  UINT32 i;

  SdaRelease ();
  for (i = 0; i < 9; i++) {
    SclLow ();     BbDelay ();
    SclRelease (); BbDelay ();
  }
  BbStop ();
}

/* register-style read (repeated start): enough to see whether the chip answers at all */
STATIC
UINT32
BbReadRegister (
  IN  UINT16 Address,
  IN  UINT8  Reg,
  OUT UINT8  *Buffer,
  IN  UINT32 Length
  )
{
  UINT32 i;

  BbStart ();
  if (!BbWriteByte ((UINT8)(Address << 1))) {
    BbStop ();
    return 0;
  }
  BbWriteByte (Reg);
  BbStart ();
  if (!BbWriteByte ((UINT8)((Address << 1) | 1))) {
    BbStop ();
    return 0;
  }
  for (i = 0; i < Length; i++) {
    Buffer[i] = BbReadByte (i + 1 < Length);
  }
  BbStop ();
  return 1;
}

/* ---- FTM5 ----------------------------------------------------------------------------------- */

/* the kernel's ftm5_read(): command without a stop, then the read starts again */
STATIC
BOOLEAN
Ftm5Read (
  IN  CONST UINT8 *Command,
  IN  UINT32      Count,
  OUT UINT8       *Data,
  IN  UINT32      Length
  )
{
  if (EFI_ERROR (BbWriteStretch (I2C_ADDR_ST, Command, Count))) {
    BbRecover ();
    return FALSE;
  }
  if (EFI_ERROR (BbRead (I2C_ADDR_ST, Data, Length))) {
    BbRecover ();
    return FALSE;
  }
  return TRUE;
}

STATIC
BOOLEAN
Ftm5Wait (
  IN UINT8   Want,
  IN UINT32  TimeoutMs,
  OUT UINT8 *Event
  )
{
  UINT8  cmd[1] = { FTM5_READ_EVENT };
  UINT32 waited;

  for (waited = 0; waited < TimeoutMs; waited += 10) {
    if (Ftm5Read (cmd, 1, Event, 8) && Event[0] == Want) {
      return TRUE;
    }
    gBS->Stall (10000);
  }
  return FALSE;
}

STATIC
BOOLEAN
Ftm5WaitBytes (
  IN  CONST UINT8 *Want,
  IN  UINT32      Count,
  IN  UINT32      TimeoutMs,
  OUT UINT8       *Event
  )
{
  UINT8  cmd[1] = { FTM5_READ_EVENT };
  UINT32 waited;

  for (waited = 0; waited < TimeoutMs; waited += 10) {
    if (Ftm5Read (cmd, 1, Event, 8) && CompareMem (Event, Want, Count) == 0) {
      return TRUE;
    }
    gBS->Stall (10000);
  }
  return FALSE;
}

/* a command with a stop, answered by a status event that echoes it */
STATIC
BOOLEAN
Ftm5Command (
  IN CONST UINT8 *Command,
  IN UINT32      Length
  )
{
  UINT8 want[8] = { 0x43, 0x01, 0, 0, 0, 0, 0, 0 };
  UINT8 echo[8];
  UINT32 n = Length > 5 ? 5 : Length;

  if (EFI_ERROR (BbWrite (I2C_ADDR_ST, Command, Length))) {
    BbRecover ();
    return FALSE;
  }
  CopyMem (want + 2, Command, n);
  return Ftm5WaitBytes (want, 2 + n, 500, echo);
}
EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;
  BOOLEAN    found = FALSE;
  UINT8      ev[8];

  Print (L"\n=== perseus touch probe v4 (UEFI) ===\n");

  Status = gBS->LocateProtocol (&gQcomTlmmProtocolGuid, NULL, (VOID **)&mTlmm);
  Print (L"TLMM: %r\n", Status);
  if (EFI_ERROR (Status)) {
    goto hold;
  }
  Status = gBS->LocateProtocol (&gQcomI2cProtocolGuid, NULL, (VOID **)&mI2c);
  Print (L"I2C : %r\n", Status);
  if (EFI_ERROR (Status)) {
    goto hold;
  }
  Status = gBS->LocateProtocol (&gQcomPmicVregProtocolGuid, NULL, (VOID **)&mVreg);
  Print (L"VREG: %r (L28 left alone: this firmware has no other user of it)\n", Status);

  /* 1. the I/O rail, disp_vddio_vreg on GPIO79 */
  Print (L"before: gpio79=%u\n", PinLevel (PIN_VDDIO));
  Status = Gpio (PIN_VDDIO, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_HIGH_VALUE);
  Print (L"gpio79 high: %r, reads %u\n", Status, PinLevel (PIN_VDDIO));
  gBS->Stall (5000);

  /* 2. reset, and the interrupt line as an input */
  Gpio (PIN_RESET, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_LOW_VALUE);
  gBS->Stall (10000);
  Gpio (PIN_INT, GPIO_INPUT, GPIO_PULL_UP, 0);
  Gpio (PIN_RESET, GPIO_OUTPUT, GPIO_PULL_UP, GPIO_HIGH_VALUE);
  Print (L"reset released, gpio79=%u\n", PinLevel (PIN_VDDIO));
  gBS->Stall (50000);

  /* 3. does the chip answer at 0x49? */
  {
    UINT8 id[2] = { 0, 0 };
    if (BbReadRegister (I2C_ADDR_ST, REG_CHIP_ID, id, 2)) {
      Print (L"0x49 answers: %02x %02x\n", id[0], id[1]);
      found = TRUE;
    } else {
      Print (L"0x49 does not answer\n");
    }
    BbRecover ();
  }

  /* 4. FTM5: wait for "controller ready" (an event, read the FTM5 way) */
  if (Ftm5Wait (EVT_CONTROLLER_READY, 2000, ev)) {
    Print (L"FTM5: controller ready: %02x %02x %02x %02x %02x\n", ev[0], ev[1], ev[2], ev[3], ev[4]);
    found = TRUE;
  } else {
    Print (L"FTM5: no controller ready within 2 s\n");
  }

  /* 5. the chip id and the system info */
  {
    UINT8 cmd[5], b[32];

    cmd[0] = FTM5_HW_REG; cmd[1] = 0x20; cmd[2] = 0x00; cmd[3] = 0x00; cmd[4] = 0x00;
    if (Ftm5Read (cmd, 5, b, 8)) {
      Print (L"FTM5 chip id: %02x %02x %02x %02x %02x %02x (36 39 = ST FTS5)\n", b[0], b[1], b[2], b[3], b[4], b[5]);
      if (b[0] == 0x36 && b[1] == 0x39) {
        found = TRUE;
      }
    }
    cmd[0] = FTM5_FRAMEBUFFER; cmd[1] = 0x00; cmd[2] = 0x00;
    if (Ftm5Read (cmd, 3, b, 32)) {
      Print (L"FTM5 sysinfo: type %02x api %u.%u chip0 %04x fw %04x cfg %04x\n", b[1], b[7], b[6],
             (UINT32)(b[10] | (b[11] << 8)), (UINT32)(b[16] | (b[17] << 8)), (UINT32)(b[20] | (b[21] << 8)));
      found = TRUE;
    }
  }

  /* 6. scanning on, then the events: touch the panel and watch them arrive */
  {
    UINT8  scan[3] = { 0xA0, 0x00, 0x01 };
    UINT32 n, printed = 0;

    if (Ftm5Command (scan, 3)) {
      Print (L"FTM5: scanning on (touch the panel now)\n");
    } else {
      Print (L"FTM5: scanning not confirmed, watching anyway\n");
    }
    for (n = 0; n < 1500 && printed < 40; n++) {
      UINT8  cmd[1] = { FTM5_READ_EVENT };
      UINT8  ev[8];
      UINT8  raw[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
      UINT32 intr = PinLevel (PIN_INT);
      BOOLEAN got = Ftm5Read (cmd, 1, raw, 8);

      CopyMem (ev, raw, 8);
      if (got && ev[0] != 0) {
        UINT32 finger = ev[1] >> 4;
        UINT32 type   = ev[1] & 0xf;
        UINT32 cx     = (UINT32)(ev[3] & 0x0f) << 8 | ev[2];
        UINT32 cy     = (UINT32)ev[4] << 4 | (ev[3] >> 4);
        Print (L"evt %02x f%u t%u x%u y%u (int %u)\n", ev[0], finger, type, cx, cy, intr);
        printed++;
      } else if ((n % 50) == 0) {
        /* every second: the interrupt line and the raw answer tell a dead analog front end */
        Print (L"idle: int=%u read=%u raw=%02x %02x %02x %02x %02x %02x %02x %02x\n",
               intr, (UINT32)got, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
      }
      gBS->Stall (20000);
    }
    Print (L"events printed: %u\n", printed);
  }
  Print (L"=== probe done: %s ===\n", found ? L"a touch controller answered" : L"nothing answered");

hold:
  Print (L"\nholding the screen (power off to leave)\n");
  for (;;) {
    gBS->Stall (1000000);
  }
}
