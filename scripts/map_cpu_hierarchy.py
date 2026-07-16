#!/usr/bin/env python3
import os
import sys
import re
import argparse

# Comprehensive mapping of ARM Implementer codes (MIDR bits [31:24])
IMPLEMENTERS = {
    0x41: "ARM",
    0x42: "Broadcom",
    0x43: "Cavium/Marvell",
    0x44: "DEC",
    0x46: "Fujitsu",
    0x48: "HiSilicon",
    0x49: "Huawei",
    0x4d: "NXP/Motorola",
    0x50: "Applied Micro",
    0x51: "Qualcomm",
    0x53: "Samsung",
    0x56: "Marvell",
    0x61: "Apple",
    0x66: "Intel",
    0x69: "Intel",
    0x6d: "Microsoft",
    0xc0: "Ampere",
}

# Mapping of specific Part Numbers (MIDR bits [15:4]) per implementer
ARM_PARTS = {
    # ARM (0x41)
    (0x41, 0x810): "ARM810",
    (0x41, 0x920): "ARM920",
    (0x41, 0x922): "ARM922",
    (0x41, 0x926): "ARM926",
    (0x41, 0x940): "ARM940",
    (0x41, 0x946): "ARM946",
    (0x41, 0x966): "ARM966",
    (0x41, 0xa20): "ARM1020",
    (0x41, 0xa22): "ARM1022",
    (0x41, 0xa26): "ARM1026",
    (0x41, 0xb02): "ARM11 MPCore",
    (0x41, 0xb36): "ARM1136",
    (0x41, 0xb56): "ARM1156",
    (0x41, 0xb76): "ARM1176",
    (0x41, 0xc05): "Cortex-A5",
    (0x41, 0xc07): "Cortex-A7",
    (0x41, 0xc08): "Cortex-A8",
    (0x41, 0xc09): "Cortex-A9",
    (0x41, 0xc0d): "Cortex-A12",
    (0x41, 0xc0e): "Cortex-A17",
    (0x41, 0xc0f): "Cortex-A15",
    (0x41, 0xc14): "Cortex-R4",
    (0x41, 0xc15): "Cortex-R5",
    (0x41, 0xc17): "Cortex-R7",
    (0x41, 0xc18): "Cortex-R8",
    (0x41, 0xc20): "Cortex-M0",
    (0x41, 0xc21): "Cortex-M1",
    (0x41, 0xc23): "Cortex-M3",
    (0x41, 0xc24): "Cortex-M4",
    (0x41, 0xc27): "Cortex-M7",
    (0x41, 0xc60): "Cortex-M0+",
    (0x41, 0xd01): "Cortex-A32",
    (0x41, 0xd03): "Cortex-A53",
    (0x41, 0xd04): "Cortex-A35",
    (0x41, 0xd05): "Cortex-A55",
    (0x41, 0xd07): "Cortex-A57",
    (0x41, 0xd08): "Cortex-A72",
    (0x41, 0xd09): "Cortex-A73",
    (0x41, 0xd0a): "Cortex-A75",
    (0x41, 0xd0b): "Cortex-A76",
    (0x41, 0xd0c): "Neoverse-N1",
    (0x41, 0xd0d): "Cortex-A77",
    (0x41, 0xd0e): "Cortex-A76AE",
    (0x41, 0xd20): "Cortex-M23",
    (0x41, 0xd21): "Cortex-M33",
    (0x41, 0xd40): "Neoverse-V1",
    (0x41, 0xd41): "Cortex-A78",
    (0x41, 0xd42): "Cortex-A78AE",
    (0x41, 0xd43): "Cortex-A78C",
    (0x41, 0xd44): "Cortex-X1",
    (0x41, 0xd46): "Cortex-A510",
    (0x41, 0xd47): "Cortex-A710",
    (0x41, 0xd48): "Cortex-X2",
    (0x41, 0xd49): "Neoverse-N2",
    (0x41, 0xd4a): "Neoverse-V2",
    (0x41, 0xd4b): "Cortex-A78C",
    (0x41, 0xd4c): "Cortex-X1C",
    (0x41, 0xd4d): "Cortex-A715",
    (0x41, 0xd4e): "Cortex-X3",
    (0x41, 0xd80): "Cortex-A520",
    (0x41, 0xd81): "Cortex-A720",
    (0x41, 0xd82): "Cortex-X4",
    (0x41, 0xd84): "Neoverse-V3",
    (0x41, 0xd85): "Neoverse-N3",
    (0x41, 0xd87): "Cortex-X925",
    (0x41, 0xd8e): "Cortex-A725",
    (0x41, 0xd8f): "Cortex-A525",

    # Apple (0x61)
    (0x61, 0x020): "Apple M1 Icestorm",
    (0x61, 0x021): "Apple M1 Firestorm",
    (0x61, 0x022): "Apple M1 Icestorm Pro/Max/Ultra",
    (0x61, 0x023): "Apple M1 Firestorm Pro/Max/Ultra",
    (0x61, 0x024): "Apple M1 Icestorm Pro",
    (0x61, 0x025): "Apple M1 Firestorm Pro",
    (0x61, 0x028): "Apple M1 Icestorm Max",
    (0x61, 0x029): "Apple M1 Firestorm Max",
    (0x61, 0x030): "Apple M2 Blizzard",
    (0x61, 0x031): "Apple M2 Avalanche",
    (0x61, 0x032): "Apple M2 Blizzard Pro/Max/Ultra",
    (0x61, 0x033): "Apple M2 Avalanche Pro/Max/Ultra",
    (0x61, 0x034): "Apple M2 Blizzard Pro",
    (0x61, 0x035): "Apple M2 Avalanche Pro",
    (0x61, 0x038): "Apple M2 Blizzard Max",
    (0x61, 0x039): "Apple M2 Avalanche Max",
    (0x61, 0x042): "Apple M3 Sawtooth",
    (0x61, 0x043): "Apple M3 Ibiza",
    (0x61, 0x044): "Apple M3 Sawtooth Pro",
    (0x61, 0x045): "Apple M3 Ibiza Pro",
    (0x61, 0x048): "Apple M3 Sawtooth Max",
    (0x61, 0x049): "Apple M3 Ibiza Max",
    (0x61, 0x052): "Apple M4 Efes",
    (0x61, 0x053): "Apple M4 Coll",

    # Cavium/Marvell (0x43)
    (0x43, 0x0a1): "ThunderX",
    (0x43, 0x0a2): "ThunderX 81xx",
    (0x43, 0x0a3): "ThunderX 83xx",
    (0x43, 0x0af): "ThunderX2",
    (0x43, 0x0b1): "OCTEON TX2",
    (0x43, 0x0b8): "ThunderX3",

    # Fujitsu (0x46)
    (0x46, 0x001): "A64FX",

    # Ampere (0xc0)
    (0xc0, 0xac3): "AmpereOne",
    (0xc0, 0xac4): "AmpereOne",
}

# Global fallback mappings (if implementer lookup is not found or matches ARM standards)
GLOBAL_PARTS = {
    0xd03: "Cortex-A53",
    0xd04: "Cortex-A35",
    0xd05: "Cortex-A55",
    0xd07: "Cortex-A57",
    0xd08: "Cortex-A72",
    0xd09: "Cortex-A73",
    0xd0a: "Cortex-A75",
    0xd0b: "Cortex-A76",
    0xd0c: "Neoverse-N1",
    0xd0d: "Cortex-A77",
    0xd40: "Neoverse-V1",
    0xd41: "Cortex-A78",
    0xd44: "Cortex-X1",
    0xd46: "Cortex-A510",
    0xd47: "Cortex-A710",
    0xd48: "Cortex-X2",
    0xd49: "Neoverse-N2",
    0xd4a: "Neoverse-V2",
    0xd4d: "Cortex-A715",
    0xd4e: "Cortex-X3",
    0xd80: "Cortex-A520",
    0xd81: "Cortex-A720",
    0xd82: "Cortex-X4",
    0xd84: "Neoverse-V3",
    0xd85: "Neoverse-N3",
    0xd87: "Cortex-X925",
    0xd8e: "Cortex-A725",
    0xd8f: "Cortex-A525",
}

def parse_cpu_list(cpu_list_str):
    """Parses a Linux CPU list string (e.g. '0-3,5,7-9') into a list of integers."""
    if not cpu_list_str:
        return []
    cpus = set()
    parts = cpu_list_str.strip().split(',')
    for part in parts:
        if not part:
            continue
        if '-' in part:
            try:
                start, end = part.split('-')
                cpus.update(range(int(start), int(end) + 1))
            except ValueError:
                pass
        else:
            try:
                cpus.add(int(part))
            except ValueError:
                pass
    return sorted(list(cpus))

def format_cpu_list(cpus):
    """Formats a list of CPU IDs into a compact string representation."""
    if not cpus:
        return ""
    cpus = sorted(list(cpus))
    ranges = []
    start = cpus[0]
    prev = cpus[0]
    for c in cpus[1:]:
        if c == prev + 1:
            prev = c
        else:
            if start == prev:
                ranges.append(str(start))
            else:
                ranges.append(f"{start}-{prev}")
            start = c
            prev = c
    if start == prev:
        ranges.append(str(start))
    else:
        ranges.append(f"{start}-{prev}")
    return ",".join(ranges)

def parse_size_to_bytes(size_str):
    """Converts a size string like '64K' or '12M' to bytes."""
    if not size_str:
        return 0
    size_str = size_str.strip().upper()
    if size_str.endswith('B'):
        size_str = size_str[:-1]
    
    if size_str.endswith('K'):
        return int(size_str[:-1]) * 1024
    elif size_str.endswith('M'):
        return int(size_str[:-1]) * 1024 * 1024
    elif size_str.endswith('G'):
        return int(size_str[:-1]) * 1024 * 1024 * 1024
    else:
        try:
            return int(size_str)
        except ValueError:
            return 0

def format_bytes(bytes_val):
    """Formats bytes into a readable KiB/MiB string."""
    if bytes_val == 0:
        return "None"
    if bytes_val >= 1024 * 1024 * 1024:
        return f"{bytes_val / (1024*1024*1024):.1f} GiB"
    elif bytes_val >= 1024 * 1024:
        return f"{bytes_val / (1024*1024):.1f} MiB"
    elif bytes_val >= 1024:
        # Check if it divides cleanly to integer
        kib = bytes_val / 1024
        if kib == int(kib):
            return f"{int(kib)} KiB"
        return f"{kib:.1f} KiB"
    else:
        return f"{bytes_val} B"

def parse_cpuinfo():
    """Parses /proc/cpuinfo to extract core properties as a dictionary of dictionaries."""
    cpus = {}
    current_cpu = None
    try:
        if os.path.exists("/proc/cpuinfo"):
            with open("/proc/cpuinfo", "r") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    if ":" in line:
                        key, val = line.split(":", 1)
                        key = key.strip()
                        val = val.strip()
                        if key == "processor":
                            try:
                                current_cpu = int(val)
                                cpus[current_cpu] = {}
                            except ValueError:
                                current_cpu = None
                        elif current_cpu is not None:
                            cpus[current_cpu][key] = val
    except Exception:
        pass
    return cpus

def get_cpu_ids():
    """Gets sorted list of CPU IDs present in /sys/devices/system/cpu."""
    cpu_dir = "/sys/devices/system/cpu"
    cpus = []
    if os.path.exists(cpu_dir):
        for name in os.listdir(cpu_dir):
            if re.match(r"^cpu\d+$", name):
                cpus.append(int(name[3:]))
    return sorted(cpus)

def get_cpu_max_freq(cpu_id):
    """Tries to retrieve the maximum frequency of a CPU in kHz."""
    paths = [
        f"/sys/devices/system/cpu/cpu{cpu_id}/cpufreq/cpuinfo_max_freq",
        f"/sys/devices/system/cpu/cpu{cpu_id}/cpufreq/scaling_max_freq",
    ]
    for path in paths:
        if os.path.exists(path):
            try:
                with open(path, "r") as f:
                    return int(f.read().strip())
            except Exception:
                pass
    return None

def get_cpu_midr(cpu_id, cpuinfo_data):
    """Retrieves the MIDR value for a CPU, falling back to /proc/cpuinfo if sysfs is restricted."""
    midr_path = f"/sys/devices/system/cpu/cpu{cpu_id}/regs/identification/midr_el1"
    try:
        if os.path.exists(midr_path):
            with open(midr_path, "r") as f:
                return int(f.read().strip(), 16)
    except Exception:
        pass
    
    # Fallback to proc cpuinfo
    info = cpuinfo_data.get(cpu_id, {})
    imp_str = info.get("CPU implementer")
    part_str = info.get("CPU part")
    var_str = info.get("CPU variant")
    rev_str = info.get("CPU revision")
    
    if imp_str and part_str:
        try:
            # Parse hex or decimal strings
            imp = int(imp_str, 16) if imp_str.lower().startswith('0x') else int(imp_str)
            part = int(part_str, 16) if part_str.lower().startswith('0x') else int(part_str)
            variant = int(var_str, 16) if var_str and var_str.lower().startswith('0x') else int(var_str or '0')
            rev = int(rev_str, 16) if rev_str and rev_str.lower().startswith('0x') else int(rev_str or '0')
            return (imp << 24) | (variant << 20) | (0xf << 16) | (part << 4) | rev
        except ValueError:
            pass
    return None

def decode_midr(midr):
    """Decodes MIDR register into implementer name, core model, and stepping revision."""
    if midr is None:
        return "Unknown Core", "N/A"
    implementer = (midr >> 24) & 0xff
    variant = (midr >> 20) & 0xf
    part = (midr >> 4) & 0xfff
    revision = midr & 0xf
    
    imp_name = IMPLEMENTERS.get(implementer, f"Unknown Implementer (0x{implementer:02x})")
    
    # Try implementer-specific part mapping first
    core_name = ARM_PARTS.get((implementer, part))
    if not core_name:
        # Fallback to global ARM list
        core_name = GLOBAL_PARTS.get(part, f"Part 0x{part:03x}")
        
    revision_str = f"r{variant}p{revision}"
    return f"{imp_name} {core_name}", revision_str

def get_cpu_topology(cpu_id):
    """Retrieves topology information (core_id, physical_package_id, thread_siblings) for a CPU from sysfs."""
    topology_dir = f"/sys/devices/system/cpu/cpu{cpu_id}/topology"
    core_id = None
    package_id = None
    thread_siblings = [cpu_id]
    
    if os.path.exists(topology_dir):
        # Read core_id
        core_id_path = os.path.join(topology_dir, "core_id")
        if os.path.exists(core_id_path):
            try:
                with open(core_id_path, "r") as f:
                    core_id = int(f.read().strip())
            except Exception:
                pass
                
        # Read physical_package_id
        package_id_path = os.path.join(topology_dir, "physical_package_id")
        if os.path.exists(package_id_path):
            try:
                with open(package_id_path, "r") as f:
                    package_id = int(f.read().strip())
            except Exception:
                pass
                
        # Read thread siblings list
        siblings_paths = [
            os.path.join(topology_dir, "thread_siblings_list"),
            os.path.join(topology_dir, "core_cpus_list"),
        ]
        for path in siblings_paths:
            if os.path.exists(path):
                try:
                    with open(path, "r") as f:
                        siblings_str = f.read().strip()
                        if siblings_str:
                            thread_siblings = parse_cpu_list(siblings_str)
                            break
                except Exception:
                    pass
    return core_id, package_id, thread_siblings

def get_core_details(cpu_id, cpuinfo_data, arch):
    """Retrieves human-readable core model and stepping based on CPU architecture."""
    is_arm = 'arm' in arch or 'aarch64' in arch
    info = cpuinfo_data.get(cpu_id, {})
    
    if is_arm:
        midr = get_cpu_midr(cpu_id, cpuinfo_data)
        model, stepping = decode_midr(midr)
        return model, stepping, midr
    else:
        # For x86 or others, extract details from cpuinfo
        model_name = info.get("model name")
        vendor_id = info.get("vendor_id", "")
        stepping = info.get("stepping")
        microcode = info.get("microcode")
        
        if model_name:
            model = model_name
        else:
            model = f"{vendor_id} CPU" if vendor_id else "Unknown CPU"
            
        step_parts = []
        if stepping:
            step_parts.append(f"stepping {stepping}")
        if microcode:
            step_parts.append(f"ucode {microcode}")
            
        stepping_str = ", ".join(step_parts) if step_parts else "N/A"
        return model, stepping_str, None

def get_cpu_caches(cpu_id):
    """Retrieves all cache descriptors for a CPU from sysfs."""
    cache_dir = f"/sys/devices/system/cpu/cpu{cpu_id}/cache"
    caches = []
    if not os.path.exists(cache_dir):
        return caches
    for name in sorted(os.listdir(cache_dir)):
        if not name.startswith("index"):
            continue
        idx_path = os.path.join(cache_dir, name)
        
        def read_file(fname):
            fpath = os.path.join(idx_path, fname)
            if os.path.exists(fpath):
                try:
                    with open(fpath, "r") as f:
                        return f.read().strip()
                except Exception:
                    pass
            return None

        level = read_file("level")
        type_str = read_file("type")
        size = read_file("size")
        shared_cpu_list = read_file("shared_cpu_list")
        ways = read_file("ways_of_associativity")
        sets = read_file("number_of_sets")
        line_size = read_file("coherency_line_size")

        if level is not None:
            caches.append({
                "index_name": name,
                "level": int(level),
                "type": type_str or "Unified",
                "size": size, # String from sysfs, e.g. "64K"
                "shared_cpu_list": shared_cpu_list,
                "ways": ways,
                "sets": sets,
                "line_size": line_size
            })
    return caches


def get_cache_sharing_rank(cpu_id, level, unique_caches, c_type_preference=["Data", "Unified", "Instruction"]):
    """Finds the rank of cpu_id within its sharing group for a cache at a given level."""
    matching_caches = []
    for (lvl, c_type, shared_cpus), c_info in unique_caches.items():
        if lvl == level and cpu_id in shared_cpus:
            matching_caches.append((c_type, shared_cpus))
            
    if not matching_caches:
        return 0
        
    chosen_shared_cpus = None
    for pref in c_type_preference:
        for c_type, shared_cpus in matching_caches:
            if c_type.lower() == pref.lower():
                chosen_shared_cpus = shared_cpus
                break
        if chosen_shared_cpus:
            break
            
    if not chosen_shared_cpus:
        chosen_shared_cpus = matching_caches[0][1]
        
    return chosen_shared_cpus.index(cpu_id)


def get_core_type_score(model_name):
    """Scores a core's capability based on model name substrings (higher is better)."""
    if not model_name:
        return 0
    model_name_lower = model_name.lower()
    
    # Prime/Super big cores
    if "cortex-x" in model_name_lower or "x925" in model_name_lower:
        return 3
        
    # Big/Performance cores
    if "cortex-a7" in model_name_lower or "neoverse" in model_name_lower:
        return 2
    if "apple" in model_name_lower and any(term in model_name_lower for term in ["firestorm", "avalanche", "sawtooth", "coll"]):
        return 2
    if "p-core" in model_name_lower or "zen 5" in model_name_lower or "zen 4" in model_name_lower:
        if "zen 5c" not in model_name_lower and "zen 4c" not in model_name_lower:
            return 2
        
    # LITTLE/Efficiency cores
    if "cortex-a5" in model_name_lower or "cortex-a3" in model_name_lower:
        return 1
    if "apple" in model_name_lower and any(term in model_name_lower for term in ["icestorm", "blizzard", "ibiza"]):
        return 1
    if "e-core" in model_name_lower or "zen 5c" in model_name_lower or "zen 4c" in model_name_lower:
        return 1
        
    return 0


def get_l3_cache_groups(cpu_ids, unique_caches, core_infos):
    """Finds L3 cache groups (falling back to L2, then single group) sorted by preference."""
    # Find all caches at level 3
    l3_caches = [c_info for key, c_info in unique_caches.items() if key[0] == 3]
    
    # Fallback to level 2
    if not l3_caches:
        l3_caches = [c_info for key, c_info in unique_caches.items() if key[0] == 2]
        
    # Fallback to single group of all CPUs
    if not l3_caches:
        groups = [tuple(cpu_ids)]
    else:
        seen_cpus = set()
        groups = []
        # Sort caches by their shared CPU list to be deterministic
        for c in sorted(l3_caches, key=lambda x: x["shared_cpus"]):
            cpus_tuple = c["shared_cpus"]
            if cpus_tuple not in seen_cpus:
                seen_cpus.add(cpus_tuple)
                groups.append(cpus_tuple)
                
    # Sort CPUs inside each group by sibling/sharing rank, core type, and frequency
    sorted_groups = []
    for group in groups:
        sorted_cpus = sorted(group, key=lambda cpu: (
            get_cache_sharing_rank(cpu, 1, unique_caches), # SMT rank (L1)
            get_cache_sharing_rank(cpu, 2, unique_caches), # L2 sharing rank
            -get_core_type_score(core_infos.get(cpu, {}).get("model", "")), # Core type (descending)
            -(core_infos.get(cpu, {}).get("max_freq") or 0), # Max freq (descending)
            cpu # Deterministic tie-breaker
        ))
        sorted_groups.append(sorted_cpus)
        
    return sorted_groups


class TreeNode:
    """Represents a node in the CPU/Cache topology hierarchy."""
    def __init__(self, node_type, name, cpu_set, level=0, size_str="", bytes_val=0, details=""):
        self.node_type = node_type  # 'cache' or 'core'
        self.name = name            # e.g., 'L3 Unified Cache' or 'Core 0'
        self.cpu_set = set(cpu_set)
        self.level = level          # Cache level (0 for core)
        self.size_str = size_str
        self.bytes_val = bytes_val
        self.details = details      # Additional info (freq, architecture, etc.)
        self.private_caches = []    # List of private caches strings
        self.children = []

def build_topology(cpu_ids, unique_caches, core_infos):
    """Builds the tree representing shared caches containing smaller caches and CPU cores."""
    all_nodes = []
    
    # 1. Create Core nodes
    core_nodes = {}
    for cpu_id in cpu_ids:
        info = core_infos[cpu_id]
        freq_str = f" @ {info['max_freq']/1e6:.2f} GHz" if info['max_freq'] else ""
        details = f"{info['model']} ({info['stepping']}){freq_str}"
        
        # Get SMT information to format node name
        core_id = info.get("core_id")
        if core_id is None:
            core_id = cpu_id
        thread_siblings = info.get("thread_siblings") or [cpu_id]
        smt_index = info.get("smt_index", 0)
        
        if len(thread_siblings) > 1:
            name = f"Core {core_id} (Thread {smt_index}, cpu{cpu_id})"
        else:
            name = f"Core {core_id} (cpu{cpu_id})"
            
        node = TreeNode(
            node_type='core',
            name=name,
            cpu_set={cpu_id},
            details=details
        )
        
        # Populate private caches for this core
        private_list = []
        for (level, c_type, shared_cpus), c_info in unique_caches.items():
            if shared_cpus == (cpu_id,):
                size_formatted = c_info["size"] or "Unreported Size"
                # Skip displaying L2 if it's completely empty / missing size
                if level == 2 and c_info["bytes"] == 0:
                    continue
                private_list.append((level, c_type, f"L{level} {c_type} ({size_formatted})"))
        
        # Sort private caches by level ascending, then by type
        private_list.sort(key=lambda x: (x[0], x[1]))
        node.private_caches = [x[2] for x in private_list]
        
        core_nodes[cpu_id] = node
        all_nodes.append(node)

    # 2. Create Shared Cache nodes (shared by more than 1 CPU)
    shared_cache_nodes = []
    for (level, c_type, shared_cpus), c_info in unique_caches.items():
        if len(shared_cpus) > 1:
            size_formatted = c_info["size"] or "Unreported Size"
            node = TreeNode(
                node_type='cache',
                name=f"L{level} {c_type} Cache",
                cpu_set=set(shared_cpus),
                level=level,
                size_str=size_formatted,
                bytes_val=c_info["bytes"],
                details=f"Shared by CPUs: {format_cpu_list(shared_cpus)}"
            )
            shared_cache_nodes.append(node)
            all_nodes.append(node)

    # 3. Associate parents and children
    # A parent of node `u` is the shared cache node `v` that contains `u`'s CPUs,
    # has the smallest cpu_set size (closest containment), and higher cache level if both are caches.
    has_parent = set()
    for u in all_nodes:
        parent = None
        for v in shared_cache_nodes:
            if u == v:
                continue
            # v must contain all of u's CPUs
            if u.cpu_set.issubset(v.cpu_set):
                # If both are caches, parent must have a strictly higher level
                if u.node_type == 'cache' and v.level <= u.level:
                    continue
                
                # Check if this v is a better parent than the current selection
                if parent is None:
                    parent = v
                else:
                    # Prefer smaller CPU set (more specific sharing cluster)
                    if len(v.cpu_set) < len(parent.cpu_set):
                        parent = v
                    elif len(v.cpu_set) == len(parent.cpu_set):
                        # Tie-breaker: prefer closer cache level
                        if u.node_type == 'cache':
                            if (v.level - u.level) < (parent.level - u.level):
                                parent = v
                        else:
                            if v.level < parent.level:
                                parent = v
        if parent:
            parent.children.append(u)
            has_parent.add(u)

    # Roots are nodes with no parents
    roots = [n for n in all_nodes if n not in has_parent]
    return roots

def print_topology_tree(node, prefix="", is_last=True, is_root=False):
    """Recursively prints the topology tree using box drawing characters."""
    if is_root:
        branch = ""
        child_prefix = ""
    else:
        branch = "└── " if is_last else "├── "
        child_prefix = prefix + ("    " if is_last else "│   ")
    
    if node.node_type == 'cache':
        size_display = f" [{node.size_str}]" if node.size_str else " [Unreported Size]"
        print(f"{prefix}{branch}{node.name}{size_display}")
    else:
        # Core
        print(f"{prefix}{branch}{node.name}: {node.details}")
        if node.private_caches:
            cache_line = ", ".join(node.private_caches)
            print(f"{child_prefix}└─ Private Caches: {cache_line}")
            
    # Sort children: caches first (ordered by level desc), then cores by CPU ID
    def sort_key(child):
        if child.node_type == 'cache':
            return (0, -child.level, sorted(list(child.cpu_set))[0])
        else:
            return (1, 0, sorted(list(child.cpu_set))[0])
            
    sorted_children = sorted(node.children, key=sort_key)
    
    for i, child in enumerate(sorted_children):
        print_topology_tree(child, child_prefix, i == len(sorted_children) - 1, is_root=False)

def get_gpuinfo():
    """Gathers GPU properties from sysfs and lspci."""
    gpus = {}
    gpu_names = {}
    try:
        import subprocess
        out = subprocess.check_output(["lspci", "-D"], stderr=subprocess.DEVNULL).decode("utf-8")
        for line in out.splitlines():
            match = re.match(r"^([0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-9a-fA-F]) (VGA compatible controller|Display controller): (.*)$", line)
            if match:
                slot, _, name = match.groups()
                # Clean up vendor names to save space
                name = name.replace("Corporation ", "").replace("Advanced Micro Devices, Inc. ", "")
                gpu_names[slot.lower()] = name
    except Exception:
        pass

    drm_dir = "/sys/class/drm"
    if not os.path.exists(drm_dir):
        return gpus
        
    for name in sorted(os.listdir(drm_dir)):
        if not re.match(r"^card\d+$", name):
            continue
            
        card_path = os.path.join(drm_dir, name)
        device_path = os.path.join(card_path, "device")
        if not os.path.exists(device_path):
            continue
            
        driver_path = os.path.join(device_path, "driver")
        driver = "Unknown"
        if os.path.exists(driver_path):
            driver = os.path.basename(os.path.realpath(driver_path))
            
        slot = ""
        uevent_path = os.path.join(device_path, "uevent")
        if os.path.exists(uevent_path):
            try:
                with open(uevent_path, "r") as f:
                    for line in f:
                        if line.startswith("PCI_SLOT_NAME="):
                            slot = line.split("=", 1)[1].strip().lower()
                            break
            except Exception:
                pass
                
        model = gpu_names.get(slot, f"PCI Device {slot}" if slot else "Unknown GPU")
        
        def read_mem_file(fname):
            fpath = os.path.join(device_path, fname)
            if os.path.exists(fpath):
                try:
                    with open(fpath, "r") as f:
                        return int(f.read().strip())
                except Exception:
                    pass
            return 0
            
        vram_total = read_mem_file("mem_info_vram_total")
        vram_used = read_mem_file("mem_info_vram_used")
        gtt_total = read_mem_file("mem_info_gtt_total")
        gtt_used = read_mem_file("mem_info_gtt_used")
        
        sclk = "N/A"
        mclk = "N/A"
        if driver == "amdgpu":
            for clk_name, clk_file in [("sclk", "pp_dpm_sclk"), ("mclk", "pp_dpm_mclk")]:
                fpath = os.path.join(device_path, clk_file)
                if os.path.exists(fpath):
                    try:
                        with open(fpath, "r") as f:
                            for line in f:
                                if "*" in line:
                                    parts = line.strip().split()
                                    if len(parts) >= 2:
                                        val = parts[1].replace("*", "").strip()
                                        if val.lower().endswith("mhz"):
                                            val = val[:-3] + " MHz"
                                        elif val.lower().endswith("ghz"):
                                            val = val[:-3] + " GHz"
                                        if clk_name == "sclk":
                                            sclk = val
                                        else:
                                            mclk = val
                                        break
                    except Exception:
                        pass
                        
        def read_str_file(fname):
            fpath = os.path.join(device_path, fname)
            if os.path.exists(fpath):
                try:
                    with open(fpath, "r") as f:
                        return f.read().strip()
                except Exception:
                    pass
            return ""
            
        link_speed = read_str_file("current_link_speed").replace("PCIe", "").strip()
        link_width = read_str_file("current_link_width")
        link_str = f"PCIe {link_speed} x{link_width}" if link_speed and link_width else "N/A"
        
        gpu_busy = -1
        busy_path = os.path.join(device_path, "gpu_busy_percent")
        if os.path.exists(busy_path):
            try:
                with open(busy_path, "r") as f:
                    gpu_busy = int(f.read().strip())
            except Exception:
                pass
                
        gpus[name] = {
            "model": model,
            "driver": driver,
            "slot": slot,
            "vram_total": vram_total,
            "vram_used": vram_used,
            "gtt_total": gtt_total,
            "gtt_used": gtt_used,
            "sclk": sclk,
            "mclk": mclk,
            "link": link_str,
            "busy": gpu_busy
        }
    return gpus

def get_total_system_memory():
    """Reads /proc/meminfo to get the total system memory in bytes."""
    try:
        with open("/proc/meminfo", "r") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    parts = line.split()
                    if len(parts) >= 2:
                        return int(parts[1]) * 1024 # Convert KiB to bytes
    except Exception:
        pass
    return 0

def get_dmi_memory_devices():
    """Tries to query dmidecode for individual DIMM properties. Returns a list of populated DIMMs."""
    devices = []
    try:
        import subprocess
        # Run dmidecode for Memory Device (Type 17)
        out = subprocess.check_output(["dmidecode", "-t", "17"], stderr=subprocess.DEVNULL).decode("utf-8")
        
        # Split by "Memory Device" blocks
        blocks = out.split("Memory Device")
        for block in blocks[1:]:
            lines = block.splitlines()
            info = {}
            for line in lines:
                line = line.strip()
                if ":" in line:
                    k, v = line.split(":", 1)
                    info[k.strip().lower()] = v.strip()
            
            size = info.get("size", "")
            if size and "no module" not in size.lower() and "installed" not in size.lower():
                # Get speed, preference: Configured Memory Speed / Configured Clock Speed -> Speed
                speed = info.get("configured memory speed") or info.get("configured clock speed") or info.get("speed") or "Unknown Speed"
                m_type = info.get("type", "Unknown Type")
                locator = info.get("locator", "Unknown Slot")
                manufacturer = info.get("manufacturer", "")
                if manufacturer.lower() in ["unknown", "no asset tag", "not specified"]:
                    manufacturer = ""
                    
                devices.append({
                    "size": size,
                    "speed": speed,
                    "type": m_type,
                    "locator": locator,
                    "manufacturer": manufacturer
                })
    except Exception:
        pass
    return devices

def main():
    parser = argparse.ArgumentParser(description="Map out CPU core and cache hierarchy on Linux systems.")
    parser.add_argument("--tree-only", action="store_true", help="Only output the topology tree.")
    parser.add_argument("--list-l3-groups", action="store_true", help="Output sorted L3 cache groups (one per line, space-separated CPU IDs).")
    parser.add_argument("--list-capability-groups", action="store_true", help="Output CPU capability groups (one per line, space-separated CPU IDs of same model/freq).")
    parser.add_argument("--list-all-cpus", action="store_true", help="Output all CPU IDs (space-separated).")
    parser.add_argument("--list-thread0-cpus", action="store_true", help="Output Thread 0 CPU IDs (space-separated).")
    args = parser.parse_args()

    import platform
    arch = platform.machine().lower()

    cpuinfo_data = parse_cpuinfo()
    cpu_ids = get_cpu_ids()
    
    if not cpu_ids:
        print("Error: No CPUs found in /sys/devices/system/cpu. Is this a Linux system?", file=sys.stderr)
        sys.exit(1)

    # 1. Gather raw CPU details and topology
    raw_infos = {}
    is_x86 = not ('arm' in arch or 'aarch64' in arch)
    amd_fam26_freqs = set()
    amd_fam25_freqs = set()
    intel_freqs = set()
    intel_smt_counts = set()
    
    for cpu_id in cpu_ids:
        model, stepping, midr = get_core_details(cpu_id, cpuinfo_data, arch)
        max_freq = get_cpu_max_freq(cpu_id)
        core_id, package_id, thread_siblings = get_cpu_topology(cpu_id)
        
        info = cpuinfo_data.get(cpu_id, {})
        vendor_id = info.get("vendor_id", "")
        
        try:
            cpu_family = int(info.get("cpu family", 0))
        except ValueError:
            cpu_family = 0
            
        raw_infos[cpu_id] = {
            "midr": midr,
            "model": model,
            "stepping": stepping,
            "max_freq": max_freq,
            "core_id": core_id,
            "package_id": package_id,
            "thread_siblings": thread_siblings,
            "vendor_id": vendor_id,
            "cpu_family": cpu_family,
        }
        
        if is_x86:
            if vendor_id == "AuthenticAMD":
                if cpu_family == 26 and max_freq:
                    amd_fam26_freqs.add(max_freq)
                elif cpu_family == 25 and max_freq:
                    amd_fam25_freqs.add(max_freq)
            elif vendor_id == "GenuineIntel":
                if max_freq:
                    intel_freqs.add(max_freq)
                if thread_siblings:
                    intel_smt_counts.add(len(thread_siblings))

    core_infos = {}
    for cpu_id in cpu_ids:
        raw = raw_infos[cpu_id]
        model = raw["model"]
        vendor_id = raw["vendor_id"]
        cpu_family = raw["cpu_family"]
        max_freq = raw["max_freq"]
        thread_siblings = raw["thread_siblings"]
        
        arch_suffix = ""
        if is_x86:
            if vendor_id == "AuthenticAMD":
                if cpu_family == 26:
                    sorted_freqs = sorted(list(amd_fam26_freqs))
                    if len(sorted_freqs) > 1 and max_freq:
                        if max_freq == sorted_freqs[-1]:
                            arch_suffix = " (Zen 5)"
                        else:
                            arch_suffix = " (Zen 5c)"
                    else:
                        arch_suffix = " (Zen 5)"
                elif cpu_family == 25:
                    sorted_freqs = sorted(list(amd_fam25_freqs))
                    if len(sorted_freqs) > 1 and max_freq:
                        if max_freq == sorted_freqs[-1]:
                            arch_suffix = " (Zen 4)"
                        else:
                            arch_suffix = " (Zen 4c)"
                    else:
                        pass
            elif vendor_id == "GenuineIntel":
                is_p_core = True
                if len(intel_smt_counts) > 1:
                    is_p_core = (len(thread_siblings) == max(intel_smt_counts))
                elif len(intel_freqs) > 1 and max_freq:
                    is_p_core = (max_freq == max(intel_freqs))
                
                if len(intel_smt_counts) > 1 or len(intel_freqs) > 1:
                    arch_suffix = " (P-core)" if is_p_core else " (E-core)"
                    
        if arch_suffix:
            model = f"{model}{arch_suffix}"
            
        smt_index = thread_siblings.index(cpu_id) if cpu_id in thread_siblings else 0
        
        core_infos[cpu_id] = {
            "midr": raw["midr"],
            "model": model,
            "stepping": raw["stepping"],
            "max_freq": max_freq,
            "core_id": raw["core_id"],
            "package_id": raw["package_id"],
            "thread_siblings": thread_siblings,
            "smt_index": smt_index
        }

    # 2. Gather Unique Cache instances
    unique_caches = {}
    for cpu_id in cpu_ids:
        caches = get_cpu_caches(cpu_id)
        for c in caches:
            shared_list_str = c["shared_cpu_list"]
            if shared_list_str is None:
                shared_cpus = (cpu_id,)
            else:
                shared_cpus = tuple(parse_cpu_list(shared_list_str))
                
            key = (c["level"], c["type"], shared_cpus)
            if key not in unique_caches:
                unique_caches[key] = {
                    "level": c["level"],
                    "type": c["type"],
                    "size": c["size"],
                    "bytes": parse_size_to_bytes(c["size"]),
                    "shared_cpus": shared_cpus,
                    "ways": c["ways"],
                    "sets": c["sets"],
                    "line_size": c["line_size"]
                }

    if args.list_l3_groups:
        groups = get_l3_cache_groups(cpu_ids, unique_caches, core_infos)
        for g in groups:
            print(" ".join(map(str, g)))
        sys.exit(0)

    if args.list_capability_groups:
        groups = {}
        for cpu_id in cpu_ids:
            info = core_infos[cpu_id]
            key = (info['model'], info['max_freq'])
            if key not in groups:
                groups[key] = []
            groups[key].append(cpu_id)
        
        # Sort groups by:
        # 1. core type score (descending)
        # 2. max frequency (descending)
        # 3. CPU model string
        sorted_keys = sorted(groups.keys(), key=lambda k: (
            -get_core_type_score(k[0]),
            -(k[1] or 0),
            k[0]
        ))
        
        for key in sorted_keys:
            print(" ".join(map(str, sorted(groups[key]))))
        sys.exit(0)

    if args.list_all_cpus:
        print(" ".join(map(str, sorted(cpu_ids))))
        sys.exit(0)

    if args.list_thread0_cpus:
        t0_cpus = []
        for cpu in cpu_ids:
            if get_cache_sharing_rank(cpu, 1, unique_caches) == 0:
                t0_cpus.append(cpu)
        print(" ".join(map(str, sorted(t0_cpus))))
        sys.exit(0)

    # 3. Print summaries if not requested tree-only
    if not args.tree_only:
        print("================================================================================")
        print(f"CPU CORE & CACHE HIERARCHY MAP ({arch.upper()})")
        print("================================================================================")
        print(f"Total Cores: {len(cpu_ids)}")
        
        # Print a Core Summary Table
        print("\nCore Details Table:")
        print(f"{'Core':<14} {'Core Model':<46} {'Stepping':<28} {'Max Freq':<10} {'L1I/L1D Cache':<16} {'L2 Cache':<10}")
        print("-" * 129)
        for cpu_id in cpu_ids:
            info = core_infos[cpu_id]
            freq_str = f"{info['max_freq']/1e3:.0f} MHz" if info['max_freq'] else "N/A"
            
            # Find cache sizes for this core
            l1i_size = "None"
            l1d_size = "None"
            l2_size = "None"
            
            for (level, c_type, shared_cpus), c_info in unique_caches.items():
                if cpu_id in shared_cpus:
                    size_formatted = c_info["size"] or "N/A"
                    if level == 1:
                        if c_type == "Instruction":
                            l1i_size = size_formatted
                        elif c_type == "Data":
                            l1d_size = size_formatted
                    elif level == 2:
                        l2_size = size_formatted
            
            l1_str = f"{l1i_size} / {l1d_size}"
            
            # Format Core column with SMT info if applicable
            core_id = info["core_id"]
            if core_id is None:
                core_id = cpu_id
            thread_siblings = info["thread_siblings"]
            smt_index = info["smt_index"]
            if len(thread_siblings) > 1:
                core_col = f"cpu{cpu_id} (c{core_id}-t{smt_index})"
            else:
                core_col = f"cpu{cpu_id} (c{core_id})"
                
            print(f"{core_col:<14} {info['model']:<46} {info['stepping']:<28} {freq_str:<10} {l1_str:<16} {l2_size:<10}")

        # Print System-wide Cache Capacity Totals
        print("\nSystem-wide Cache Capacity Summary:")
        # Group unique caches by level and type
        cache_totals = {}
        for (level, c_type, shared_cpus), c_info in unique_caches.items():
            gk = (level, c_type)
            if gk not in cache_totals:
                cache_totals[gk] = {"bytes": 0, "instances": [], "unreported_count": 0}
            if c_info["bytes"] > 0:
                cache_totals[gk]["bytes"] += c_info["bytes"]
                cache_totals[gk]["instances"].append(c_info["size"])
            else:
                cache_totals[gk]["unreported_count"] += 1
                
        for gk in sorted(cache_totals.keys()):
            level, c_type = gk
            totals = cache_totals[gk]
            instances_str = ""
            if totals["instances"]:
                # Summarize frequencies of sizes, e.g., "8x 64 KiB"
                counts = {}
                for size in totals["instances"]:
                    counts[size] = counts.get(size, 0) + 1
                inst_details = [f"{cnt}x {sz}" for sz, cnt in sorted(counts.items())]
                instances_str = ", ".join(inst_details)
            if totals["unreported_count"] > 0:
                unrep = f"{totals['unreported_count']}x unreported"
                instances_str = f"{instances_str}, {unrep}" if instances_str else unrep
                
            total_formatted = format_bytes(totals["bytes"])
            print(f" - L{level} {c_type:<11} Cache: {total_formatted:<12} (Total across {instances_str})")

        # Print System Memory Summary
        total_mem_bytes = get_total_system_memory()
        total_mem_str = format_bytes(total_mem_bytes)
        
        print("\nSystem Memory Summary:")
        print(f" - Total System Memory: {total_mem_str}")
        
        dimms = get_dmi_memory_devices()
        if dimms:
            print(f" - Installed DIMMs: {len(dimms)}")
            for d in dimms:
                man_str = f" ({d['manufacturer']})" if d["manufacturer"] else ""
                print(f"   * Slot {d['locator']}: {d['size']} {d['type']} @ {d['speed']}{man_str}")
        else:
            print(" - Installed DIMMs: Details unavailable (run with sudo/root to view DIMMs & speed)")

        gpus = get_gpuinfo()
        if gpus:
            print("\n================================================================================")
            print("GPU DETAILS & PROPERTIES")
            print("================================================================================")
            print(f"{'GPU':<8} {'GPU Model':<42} {'Driver':<8} {'VRAM Total/Used':<20} {'GTT Total/Used':<20} {'Engine/Mem Clk':<18} {'PCIe Link':<20} {'Load':<6}")
            print("-" * 149)
            for name in sorted(gpus.keys()):
                info = gpus[name]
                vram_total_str = format_bytes(info["vram_total"])
                vram_used_str = format_bytes(info["vram_used"])
                vram_str = f"{vram_total_str} / {vram_used_str}"
                
                gtt_total_str = format_bytes(info["gtt_total"])
                gtt_used_str = format_bytes(info["gtt_used"])
                gtt_str = f"{gtt_total_str} / {gtt_used_str}"
                
                clk_str = f"{info['sclk']} / {info['mclk']}"
                busy_str = f"{info['busy']}%" if info["busy"] >= 0 else "N/A"
                
                model = info["model"]
                if len(model) > 42:
                    model = model[:39] + "..."
                    
                print(f"{name:<8} {model:<42} {info['driver']:<8} {vram_str:<20} {gtt_str:<20} {clk_str:<18} {info['link']:<20} {busy_str:<6}")

        print("\n================================================================================")
        print("TOPOLOGY PLUMBING TREE")
        print("================================================================================")

    roots = build_topology(cpu_ids, unique_caches, core_infos)
    for root in roots:
        print_topology_tree(root, is_root=True)

if __name__ == "__main__":
    main()
