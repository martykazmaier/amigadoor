import sys, struct
d = open(sys.argv[1], 'rb').read(); p = 0
while p < len(d) and d[p]:
    hs = d[p]; lvl = d[p+20]; comp = struct.unpack('<I', d[p+7:p+11])[0]; orig = struct.unpack('<I', d[p+11:p+15])[0]
    if lvl in (0, 1):
        nl = d[p+21]; name = d[p+22:p+22+nl].decode('latin1')
        print(f'{orig:9d} {name}')
        p += hs + 2 + comp
    else:
        print('level', lvl); break
