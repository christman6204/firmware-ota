import io
import re
import os

# Read original project
src = r'Project/RVMDK（uv5）/Fire_uCOS.uvprojx'
dst = r'Project/RVMDK（uv5）/Bootloader.uvprojx'

with io.open(src, encoding='utf-8') as f:
    xml = f.read()

# 1. Remove all RTOS source file groups (uCOS-III, uC-CPU, uC-LIB)
#    These are <Group> blocks containing <GroupName> with RTOS names
#    Remove entire <Group>...</Group> for uCOS-III, uC-CPU, uC-LIB paths
groups_to_remove = [
    r'<Group>.*?<GroupName>uCOS-III.*?</Group>',
    r'<Group>.*?<GroupName>uC-CPU.*?</Group>',
    r'<Group>.*?<GroupName>uC-LIB.*?</Group>',
    r'<Group>.*?<GroupName>APP.*?</Group>',   # Remove uCOS app files
]

for pattern in groups_to_remove:
    xml_seg = xml
    # Use DOTALL for multiline matching
    xml = re.sub(pattern, '', xml, flags=re.DOTALL)
    if len(xml) < len(xml_seg):
        print('Removed group matching: ' + pattern[:60])

# 2. Remove all Targets except the first one, rename it to "Bootloader"
#    Find all <Target>...</Target> blocks
targets = list(re.finditer(r'<Target>.*?</Target>', xml, re.DOTALL))
if targets:
    first_target = targets[0].group()
    # Remove extra targets
    for t in reversed(targets[1:]):
        xml = xml[:t.start()] + xml[t.end():]
    print(f'Kept 1 target, removed {len(targets)-1} targets')

# 3. Update target name
xml = re.sub(r'<TargetName>.*?</TargetName>', '<TargetName>Bootloader</TargetName>', xml, count=1)

# 4. Update scatter file reference
xml = re.sub(r'<ScatterFile>.*?</ScatterFile>',
    r'<ScatterFile>..\\..\\bootloader\\bootloader.sct</ScatterFile>', xml, count=1)

# 5. Update output directory and name
xml = re.sub(r'<OutputDirectory>.*?</OutputDirectory>',
    r'<OutputDirectory>.\Objects\</OutputDirectory>', xml, count=1)
xml = re.sub(r'<OutputName>.*?</OutputName>',
    r'<OutputName>bootloader</OutputName>', xml, count=1)

# 6. Remove OS-related defines (DEBUG_APP, BT_APP, UPDATA_APP)
#    Keep: STM32F10X_HD, USE_STDPERIPH_DRIVER
for define_str in ['DEBUG_APP', 'BT_APP', 'UPDATA_APP']:
    pattern = r'<Define>' + define_str + r'</Define>'
    xml = re.sub(pattern, '', xml)

# 7. Clean up include paths - remove RTOS paths, add bootloader/src
old_inc = r'<IncludePath>..\\..\\Libraries\\CMSIS;..\\..\\Libraries\\FWlib\\inc;..\\..\\User;..\\..\\User\\APP;..\\..\\User\\BSP;..\\..\\User\\uC-CPU;..\\..\\User\\uC-CPU\\ARM-Cortex-M3\\RealView;..\\..\\User\\uC-LIB;..\\..\\User\\uCOS-III\\Source;..\\..\\User\\uCOS-III\\Ports\\ARM-Cortex-M3\\Generic\\RealView</IncludePath>'
new_inc = r'<IncludePath>..\\..\\Libraries\\CMSIS;..\\..\\Libraries\\FWlib\\inc;..\\..\\bootloader\\src</IncludePath>'
xml = xml.replace(old_inc, new_inc)

# 8. Keep only CMSIS, FWlib, and startup groups (remove USER groups)
#    Remove groups that include User/ files (APP, BSP, uCOS, etc.)
user_groups = [
    r'<Group>.*?<GroupName>USER.*?</Group>',
    r'<Group>.*?<GroupName>BSP.*?</Group>',
    r'<Group>.*?<GroupName>uCOS-III\\Ports.*?</Group>',
    r'<Group>.*?<GroupName>uCOS-III\\Source.*?</Group>',
    r'<Group>.*?<GroupName>uC-CPU.*?</Group>',
    r'<Group>.*?<GroupName>uC-LIB.*?</Group>',
]
for pattern in user_groups:
    xml_seg = xml
    xml = re.sub(pattern, '', xml, flags=re.DOTALL)
    if len(xml) < len(xml_seg):
        print('Removed group matching: ' + pattern[:60])

# 9. Add bootloader file group before </Groups>
bootloader_group = '''
      <Group>
        <GroupName>Bootloader</GroupName>
        <Files>
          <File>
            <FileName>main.c</FileName>
            <FileType>1</FileType>
            <FilePath>..\\..\\bootloader\\src\\main.c</FilePath>
          </File>
          <File>
            <FileName>bsp.c</FileName>
            <FileType>1</FileType>
            <FilePath>..\\..\\bootloader\\src\\bsp.c</FilePath>
          </File>
          <File>
            <FileName>stm32f10x_it.c</FileName>
            <FileType>1</FileType>
            <FilePath>..\\..\\bootloader\\src\\stm32f10x_it.c</FilePath>
          </File>
        </Files>
      </Group>
'''
xml = xml.replace('</Groups>', bootloader_group + '\n    </Groups>')

# 10. Fix VECT_TAB_OFFSET for bootloader (must be 0)
#     system_stm32f10x.c has #ifdef for VECT_TAB_OFFSET based on DEBUG_APP/BT_APP/UPDATA_APP
#     Since we removed those defines, system_stm32f10x.c default branch sets VECT_TAB_OFFSET=0. ✓

# 11. Update memory configuration for bootloader
#     IRAM: 0x20000000 0x10000 (64KB) - same
#     IROM: bootloader size is 48KB, but flash driver sees full chip
#     Cpu line: keep same (IRAM 64KB, IROM 512KB)
#     FlashDriverDll: keep same (uses full chip flash algo)

# Write output
with io.open(dst, 'w', encoding='utf-8') as f:
    f.write(xml)

print(f'Bootloader project written to: {dst}')
print(f'Size: {len(xml)} bytes')

# Verify key settings
checks = {
    'Target name Bootloader': '<TargetName>Bootloader</TargetName>' in xml,
    'Scatter file': 'bootloader.sct' in xml,
    'Include path': 'bootloader\\\\src' in xml,
    'No OS source': 'uCOS-III' not in xml or xml.count('uCOS-III') < 2,
    'Bootloader source': 'bootloader\\\\src\\\\main.c' in xml,
    'Device STM32F103VE': 'STM32F103VE' in xml,
}
for k, v in checks.items():
    print(f'  {"OK" if v else "MISS"}: {k}')
