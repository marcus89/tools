import asyncio
import re


CARD = "1"

event_pattern = re.compile(
    r"node (?P<card>[^,]+), #(?P<numid>\d+) "
    r"\((?P<iface>\d+),(?P<device>\d+),(?P<subdevice>\d+),"
    r"(?P<name>.*),(?P<index>\d+)\) "
    r"(?P<event>\w+)"
)

def parse_event(line):
    m = event_pattern.match(line)

    if not m:
        return None

    return m.groupdict()

import re


def parse_amixer(text):
    result = {}

    # First line:
    # numid=10,iface=MIXER,name='Master Playback Volume'
    m = re.search(
        r"numid=(\d+),iface=([^,]+),name='([^']+)'",
        text
    )
    if m:
        result["numid"] = int(m.group(1))
        result["iface"] = m.group(2)
        result["name"] = m.group(3)

    # Type/range:
    # ; type=INTEGER,access=rw---R--,values=1,min=0,max=87,step=0
    m = re.search(
        r"type=([^,]+).*?min=(-?\d+),max=(-?\d+)",
        text,
        re.DOTALL
    )
    if m:
        result["type"] = m.group(1)
        result["min"] = int(m.group(2))
        result["max"] = int(m.group(3))

    # Values:
    # : values=0
    # or
    # : values=50,45
    m = re.search(
        r"\n\s*: values=([0-9,\s-]+)",
        text
    )
    if m:
        values = [
            int(x.strip())
            for x in m.group(1).split(",")
        ]
        result["values"] = values

    m = re.search(
        r"dBscale-min=([-\d.]+)dB,step=([-\d.]+)dB",
        text,
    )

    if m:
        result["min_db"] = float(m.group(1))
        result["step_db"] = float(m.group(2))

    return result

async def get_control(numid):
    """Read a control value using amixer."""
    proc = await asyncio.create_subprocess_exec(
        "amixer",
        "-c", CARD,
        "cget",
        f"numid={numid}",
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )

    stdout, _ = await proc.communicate()

    text = stdout.decode()

    control = parse_amixer(text)
    value = float(control["values"][0])
    min_value = float(control["min"])
    max_value = float(control["max"])
    mindb = float(control["min_db"])
    stepdb = float(control["step_db"])

    dB = mindb + value * stepdb
    percent = (dB - mindb) / (0 - mindb) * 100
    print(f"db: {dB} percent: {percent}")
    # if m:
        # values = [int(x) for x in m.group(1).split(",")]
        # return values

    return None


async def set_control(numid, value):
    """Set a control value."""
    proc = await asyncio.create_subprocess_exec(
        "amixer",
        "-c", CARD,
        "cset",
        f"numid={numid}",
        str(value),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )

    await proc.communicate()


async def save_state():
    """Save ALSA state."""
    proc = await asyncio.create_subprocess_exec(
        "alsactl",
        "store",
        CARD,
    )

    await proc.wait()


async def monitor_controls():
    """Listen for ALSA events."""
    proc = await asyncio.create_subprocess_exec(
        "alsactl",
        "monitor",
        f"hw:{CARD}",
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )

    current_numid = None

    async for line in proc.stdout:
        line = line.decode().strip()

        if not line:
            continue

        event = parse_event(line)
        current_numid = event["numid"]
        value = await get_control(current_numid)

        print(
            f"Control {current_numid} value = {value}"
        )


async def main():
    monitor_task = asyncio.create_task(
        monitor_controls()
    )

    # Example:
    await asyncio.sleep(5)

    # Change Master Volume:
    # await set_control(12, "50%")
    #
    # Save:
    # await save_state()

    await monitor_task


if __name__ == "__main__":
    asyncio.run(main())