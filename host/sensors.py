"""PC sensor reading.

CPU load + RAM come from psutil (no extra setup). GPU load/temp, CPU temp, VRAM,
and power come from LibreHardwareMonitor (pythonnet + its DLLs in this folder, run
as admin). read_stats() returns a dict with any of: cpu, ram, gpu, ct, gt, vr, cp, gp.
"""
import os
import sys

try:
    import psutil
except ImportError:
    psutil = None


def _base_dir():
    # When frozen by PyInstaller (onefile), bundled DLLs live under sys._MEIPASS.
    return getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))


class _LHM:
    """LibreHardwareMonitor via pythonnet, if available (.ok stays False if not)."""

    def __init__(self):
        self.ok = False
        self.err = None
        try:
            import clr  # noqa: F401  (pythonnet)
            from System import AppDomain, ResolveEventHandler
            from System.Reflection import Assembly, AssemblyName
            folder = _base_dir()

            def _resolve(sender, args):   # find System.Memory.dll etc. in our folder
                name = AssemblyName(args.Name).Name + ".dll"
                path = os.path.join(folder, name)
                return Assembly.LoadFrom(path) if os.path.exists(path) else None

            self._resolver = ResolveEventHandler(_resolve)   # keep a ref (avoid GC)
            AppDomain.CurrentDomain.AssemblyResolve += self._resolver

            Assembly.LoadFrom(os.path.join(folder, "LibreHardwareMonitorLib.dll"))
            from LibreHardwareMonitor.Hardware import Computer
            self.c = Computer()
            self.c.IsCpuEnabled = True
            self.c.IsGpuEnabled = True
            self.c.Open()
            self.ok = True
        except Exception as e:           # noqa: BLE001
            self.err = e

    def read(self):
        """Dict with any wire keys: ct, gt, gpu, vr, cp, gp."""
        from LibreHardwareMonitor.Hardware import SensorType
        out = {}
        vram_used = None
        vram_total = None
        ct_prio = -1   # track best temp sensor found so far (higher = better)
        gt_prio = -1

        def put(k, v):
            if k not in out:
                out[k] = int(v)

        for hw in self.c.Hardware:
            hw.Update()
            htype = str(hw.HardwareType)
            is_cpu = "Cpu" in htype
            is_gpu = "Gpu" in htype
            for s in hw.Sensors:
                if s.Value is None:
                    continue
                st, nm = s.SensorType, s.Name
                nm_l = nm.lower()
                if st == SensorType.Temperature:
                    if is_cpu:
                        # Prefer "Package" (whole-die average) over individual cores
                        prio = 1 if "package" in nm_l else 0
                        if prio > ct_prio:
                            out["ct"] = int(s.Value)
                            ct_prio = prio
                    elif is_gpu:
                        # Prefer "GPU Core" over hotspot / memory junction sensors
                        prio = 1 if "core" in nm_l else 0
                        if prio > gt_prio:
                            out["gt"] = int(s.Value)
                            gt_prio = prio
                elif st == SensorType.Load:
                    if is_gpu and "Core" in nm:
                        put("gpu", s.Value)
                elif st in (SensorType.SmallData, SensorType.Data):
                    # SmallData = MB, Data = GB — both give used/total VRAM
                    if is_gpu:
                        if "used" in nm_l and "memory" in nm_l and vram_used is None:
                            vram_used = float(s.Value)
                        elif "total" in nm_l and "memory" in nm_l and vram_total is None:
                            vram_total = float(s.Value)
                elif st == SensorType.Power:
                    if is_cpu and ("Package" in nm or "CPU" in nm):
                        put("cp", s.Value)
                    elif is_gpu:
                        put("gp", s.Value)

        if vram_used is not None and vram_total and vram_total > 0:
            out["vr"] = int(vram_used / vram_total * 100)
        return out


_lhm = None


def init():
    """Prime sensors; returns (lhm_ok, lhm_error)."""
    global _lhm
    if psutil:
        psutil.cpu_percent()             # first call is always 0.0; prime it
    _lhm = _LHM()
    return _lhm.ok, _lhm.err


def read_stats():
    """Dict with any of cpu/ram/gpu/ct/gt/vr/cp/gp (ints)."""
    vals = {}
    if psutil:
        vals["cpu"] = int(psutil.cpu_percent())
        vals["ram"] = int(psutil.virtual_memory().percent)
    if _lhm and _lhm.ok:
        try:
            vals.update(_lhm.read())
        except Exception:                # noqa: BLE001
            pass
    return vals


def encode(vals):
    """Serialize a stats dict to the wire format: 'cpu:45 ram:62 ...'."""
    return " ".join(f"{k}:{v}" for k, v in vals.items())
