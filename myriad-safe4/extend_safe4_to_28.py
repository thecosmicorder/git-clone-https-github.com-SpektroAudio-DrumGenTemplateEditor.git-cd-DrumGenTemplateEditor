from pathlib import Path
import math, random

ROOT=Path('Myriad')

def norm(v):
    lo,hi=min(v),max(v)
    if hi==lo:return [1024]*len(v)
    return [max(0,min(2048,round((x-lo)/(hi-lo)*2048))) for x in v]

def harm(ws, phases=None):
    phases=phases or [0.0]*len(ws)
    out=[]
    for i in range(256):
        t=i/256.0
        out.append(sum(w*math.sin(2*math.pi*(h+1)*t+phases[h]) for h,w in enumerate(ws)))
    return norm(out)

def arr(name,v):
    rows=[]
    for i in range(0,256,16): rows.append('  '+', '.join(str(x) for x in v[i:i+16])+',')
    return f'inline static constexpr uint16_t {name}[256] = {{\n'+'\n'.join(rows)+'\n};\n'

# 17 SUP: saw spectrum -> phase-smeared supersaw-like spectrum
supA=harm([1/(h+1) for h in range(12)])
supB=harm([1/(h+1) for h in range(12)],[0,.08,-.11,.16,-.19,.24,-.29,.33,-.37,.41,-.45,.49])
# 18 PHS: sine -> phase-distorted sine
phsA=norm([math.sin(2*math.pi*i/256) for i in range(256)])
phsB=[]
for i in range(256):
    t=i/256; p=.23; q=.5*(t/p) if t<p else .5+.5*((t-p)/(1-p)); phsB.append(math.sin(2*math.pi*q))
phsB=norm(phsB)
# 19 ADD: sparse -> rich additive
addA=harm([1,.12,.04]); addB=harm([1,.70,.50,.35,.25,.18,.12,.08])
# 20 SUB: fundamental-heavy -> octave-heavy stack
subA=harm([1,.28,.04,.10]); subB=harm([1,.72,.08,.32])
# 21 SYN: hard-sync-like ratios 2 -> 5
synA=norm([math.sin(2*math.pi*((2*i/256)%1)) for i in range(256)])
synB=norm([math.sin(2*math.pi*((5*i/256)%1)) for i in range(256)])
# 22 PWM: square -> narrow pulse, with a light octave edge
def pwm(width,octmix):
    o=[]
    for i in range(256):
        t=i/256; a=1 if t<width else -1; b=1 if ((2*t)%1)<width else -1; o.append(a+octmix*b)
    return norm(o)
pwmA=pwm(.50,.08); pwmB=pwm(.18,.22)
# 23 MET: inharmonic metallic spectra
metA=norm([math.sin(2*math.pi*i/256)+.42*math.sin(2*math.pi*2.73*i/256)+.22*math.sin(2*math.pi*5.41*i/256) for i in range(256)])
metB=norm([math.sin(2*math.pi*i/256)+.52*math.sin(2*math.pi*3.11*i/256)+.31*math.sin(2*math.pi*6.87*i/256) for i in range(256)])
# 24 PLK: woody/plucked resonator spectra
plkA=harm([1,.62,.35,.18,.09,.04,.02])
plkB=harm([1,.36,.12,.44,.08,.20,.04,.09])
# 25 RED: deterministic low-passed random -> brighter random
rng=random.Random(2040); white=[rng.uniform(-1,1) for _ in range(256)]
red=[]; y=0
for x in white: y=.90*y+.10*x; red.append(y)
redA=norm(red); redB=norm([.55*white[i]+.30*white[(i-1)%256]+.15*white[(i-2)%256] for i in range(256)])
# 26 BIT: 8-level -> 3-level stepped sine
bitA=norm([round(math.sin(2*math.pi*i/256)*3.5)/3.5 for i in range(256)])
bitB=norm([round(math.sin(2*math.pi*i/256)) for i in range(256)])
# 27 ORG: drawbar-ish organ mixtures
orgA=harm([1,.55,.18,.38,.10,.22,.07,.15])
orgB=harm([1,.18,.46,.14,.34,.10,.28,.08])

pairs=[('sup',supA,supB),('phs',phsA,phsB),('add',addA,addB),('sub',subA,subB),('syn',synA,synB),('pwm',pwmA,pwmB),('met',metA,metB),('plk',plkA,plkB),('red',redA,redB),('bit',bitA,bitB),('org',orgA,orgB)]
tables=''.join(arr(n+'A',a)+arr(n+'B',b) for n,a,b in pairs)
classes='''#pragma once\n#include "safe4OscillatorModels.hpp"\nnamespace Safe11Tables {\n'''+tables+'''\n}\n\nclass safeTableMorphModel : public safe4BaseOscillatorModel {\npublic:\n  safeTableMorphModel(const uint16_t* a,const uint16_t* b):ta(a),tb(b){}\n  void ctrl(const Q16_16 v) override { int32_t r=v.raw(); if(r<0)r=0; if(r>65535)r=65535; morph=(uint32_t)r; }\n  void fillBuffer(uint32_t* out) override {\n    const uint32_t inc=safeInc(); uint32_t ph=phase; int32_t er=err; const int32_t pk=peak(); const uint32_t m=morph;\n    for(size_t i=0;i<loopLength;i++){ uint32_t word=0;\n      for(size_t bit=0;bit<32;bit++){ const uint32_t idx=(ph>>16)&0xFF; const int32_t av=ta[idx],bv=tb[idx]; const int32_t amp=av+((int64_t)(bv-av)*m>>16); const int32_t y=amp>=er?1:0; er=(y?pk:0)-amp+er; word|=(uint32_t)y; word<<=1; ph+=inc; }\n      out[i]=word; } phase=ph; err=er; updateFade();\n  }\nprivate:\n  const uint16_t* ta; const uint16_t* tb; uint32_t morph=0;\n};\n'''
models=[('SuperSaw','sup','SUP'),('Phase','phs','PHS'),('Additive','add','ADD'),('Sub','sub','SUB'),('Sync','syn','SYN'),('PWM','pwm','PWM'),('Metal','met','MET'),('Pluck','plk','PLK'),('Red','red','RED'),('Bit','bit','BIT'),('Organ','org','ORG')]
for cname,n,idn in models:
    classes+=f'''class safe4{cname}Model : public safeTableMorphModel {{ public: safe4{cname}Model():safeTableMorphModel(Safe11Tables::{n}A,Safe11Tables::{n}B){{}} String getIdentifier() override {{ return "{idn}"; }} }};\n'''
(ROOT/'arduino_libmyriad'/'oscmodels'/'safe11OscillatorModels.hpp').write_text(classes)

# Keep 13-16 DSP identical; change identifiers only for readable display labels.
h4=ROOT/'arduino_libmyriad'/'oscmodels'/'safe4OscillatorModels.hpp'; s=h4.read_text()
for old,new in [('A_sine','SIN'),('B_fold','FLD'),('C_fm','FM'),('D_vowel','VOW')]:
    assert old in s; s=s.replace(old,new)
h4.write_text(s)

om=ROOT/'arduino_libmyriad'/'oscillatorModels.hpp'; s=om.read_text(); assert 'N_OSCILLATOR_MODELS = 17;' in s
s=s.replace('N_OSCILLATOR_MODELS = 17;','N_OSCILLATOR_MODELS = 28;')
assert '#include "oscmodels/safe4OscillatorModels.hpp"' in s
s=s.replace('#include "oscmodels/safe4OscillatorModels.hpp"','#include "oscmodels/safe4OscillatorModels.hpp"\n#include "oscmodels/safe11OscillatorModels.hpp"')
om.write_text(s)

# Register model objects and slots explicitly on A and both B cores.
a=ROOT/'Myriad_A'/'Myriad_A.ino'; s=a.read_text(); assert '#define MYRIAD_VERSION "1.1.2-SAFE4"' in s
s=s.replace('#define MYRIAD_VERSION "1.1.2-SAFE4"','#define MYRIAD_VERSION "1.1.2-X28"')
anchor='static safe4VowelMorphModel          safe4VowelModels[3];'; assert anchor in s
extra='\n'.join([f'static safe4{c}Model'.ljust(39)+f'safe4{c}Models[3];' for c,_,_ in models])
s=s.replace(anchor,anchor+'\n'+extra)
slot_anchor='    allOscModels[16][i] = &safe4VowelModels[i];'; assert slot_anchor in s
regs=[]
for idx,(c,_,_) in enumerate(models,17): regs.append(f'    allOscModels[{idx}][i] = &safe4{c}Models[i];')
s=s.replace(slot_anchor,slot_anchor+'\n'+'\n'.join(regs))
a.write_text(s)

b=ROOT/'Myriad_B'/'Myriad_B.ino'; s=b.read_text()
anchor='static safe4VowelMorphModel          safe4VowelModels0[3],    safe4VowelModels1[3];'; assert anchor in s
extra='\n'.join([f'static safe4{c}Model'.ljust(39)+f'safe4{c}Models0[3], safe4{c}Models1[3];' for c,_,_ in models])
s=s.replace(anchor,anchor+'\n'+extra)
for core in ('0','1'):
    slot_anchor=f'    allOscModels{core}[16][i] = &safe4VowelModels{core}[i];'; assert slot_anchor in s
    regs=[]
    for idx,(c,_,_) in enumerate(models,17): regs.append(f'    allOscModels{core}[{idx}][i] = &safe4{c}Models{core}[i];')
    s=s.replace(slot_anchor,slot_anchor+'\n'+'\n'.join(regs))
b.write_text(s)

# Improve only fallback labels: unknown/custom IDs show up to 3 chars centered.
d=ROOT/'Myriad_A'/'displayPortal.h'; s=d.read_text()
old='''          oscModelIcons[bank][model]->setFreeFont(&FreeSansBold9pt7b);\n          oscModelIcons[bank][model]->setTextColor(TFT_BLACK, TFT_SILVER);\n          String idxstr = String(oscModelIDs[model][0]);\n          oscModelIcons[bank][model]->drawString(idxstr.c_str(),0,0);'''
new='''          oscModelIcons[bank][model]->setFreeFont(&FreeSansBold9pt7b);\n          oscModelIcons[bank][model]->setTextColor(TFT_BLACK, TFT_SILVER);\n          oscModelIcons[bank][model]->setTextDatum(MC_DATUM);\n          String idxstr = oscModelIDs[model].substring(0,3);\n          oscModelIcons[bank][model]->drawString(idxstr.c_str(),iconw>>1,iconh>>1);'''
assert old in s; s=s.replace(old,new); d.write_text(s)

# Safety assertions
assert 'N_OSCILLATOR_MODELS = 28;' in om.read_text()
assert 'MYRIAD_VERSION "1.1.2-X28"' in a.read_text()
for idx in range(28):
    assert f'allOscModels[{idx}][i]' in a.read_text()
    assert f'allOscModels0[{idx}][i]' in b.read_text()
    assert f'allOscModels1[{idx}][i]' in b.read_text()
assert 'oscBankTypes[i] < 13 ? oscBankTypes[i] : 0' in a.read_text()
print('SAFE28 extension from proven SAFE4: PASS')
