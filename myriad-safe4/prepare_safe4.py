from pathlib import Path
import math

ROOT = Path('Myriad')

def carray(name, vals, cols=16):
    rows=[]
    for i in range(0,len(vals),cols):
        rows.append('  '+', '.join(map(str,vals[i:i+cols]))+',')
    return f'inline static constexpr uint16_t {name}[{len(vals)}] = {{\n'+'\n'.join(rows)+'\n};\n'

sine=[round(1024+1023*math.sin(2*math.pi*i/256)) for i in range(256)]
weights=[
 [1,.55,.35,.16,.10,.07,.04,.03],
 [1,.24,.12,.08,.22,.42,.25,.12],
 [1,.12,.08,.16,.48,.18,.10,.06],
 [1,.70,.34,.16,.09,.05,.03,.02],
 [1,.82,.44,.20,.09,.04,.02,.01],
]
vowels=[]
for w in weights:
    raw=[sum(w[h-1]*math.sin(2*math.pi*h*i/256) for h in range(1,9)) for i in range(256)]
    lo,hi=min(raw),max(raw)
    vowels.append([round((x-lo)/(hi-lo)*2048) for x in raw])

tables=carray('sine256',sine)+''.join(carray(f'vowel{j}',v) for j,v in enumerate(vowels))

header=r'''#pragma once
#include "oscillatorModel.hpp"
namespace Safe4Tables {
__TABLES__
}

class safe4BaseOscillatorModel : public virtual oscillatorModel {
public:
  safe4BaseOscillatorModel() : oscillatorModel() {
    loopLength=16; prog=bitbybit_program; updateBufferInSyncWithDMA=true; setClockModShift(1);
  }
  pio_sm_config getBaseConfig(uint offset) override { return bitbybit_program_get_default_config(offset); }
  void reset() override { phase=0; err=0; }
protected:
  uint32_t phase=0; int32_t err=0;
  inline int32_t peak() const {
    constexpr int32_t REF=1<<11;
    return static_cast<int32_t>((static_cast<int64_t>(REF)*fadeInvTable[fadeLevel])>>16);
  }
  inline void emit(uint32_t* out,const uint16_t* tab,uint32_t inc) {
    uint32_t ph=phase; int32_t er=err; const int32_t pk=peak();
    for(size_t i=0;i<loopLength;i++) { uint32_t word=0;
      for(size_t bit=0;bit<32;bit++) { uint32_t idx=(ph>>16)&0xFF; int32_t amp=tab[idx];
        int32_t y=amp>=er?1:0; er=er-amp+y*pk; word=(word<<1)|static_cast<uint32_t>(y); ph+=inc; }
      out[i]=word; }
    phase=ph; err=er; updateFade();
  }
};

class safe4PureSineModel : public safe4BaseOscillatorModel {
public:
  void fillBuffer(uint32_t* out) override { emit(out,Safe4Tables::sine256,(256U<<16)/static_cast<uint32_t>(wavelen)); }
  String getIdentifier() override { return "A_sine"; }
};

class safe4WavefolderModel : public safe4BaseOscillatorModel {
public:
  void ctrl(const Q16_16 v) override { fold=static_cast<int32_t>((static_cast<int64_t>(v.raw())*3)>>16); }
  void fillBuffer(uint32_t* out) override {
    const uint32_t inc=(256U<<16)/static_cast<uint32_t>(wavelen); uint32_t ph=phase; int32_t er=err;
    const int32_t pk=peak(), gain=1+fold;
    for(size_t i=0;i<loopLength;i++){ uint32_t word=0;
      for(size_t bit=0;bit<32;bit++){ uint32_t idx=(ph>>16)&0xFF; int32_t x=(int32_t)Safe4Tables::sine256[idx]-1024; x*=gain;
        while(x>1024||x<-1024){ if(x>1024)x=2048-x; if(x<-1024)x=-2048-x; }
        int32_t amp=x+1024,y=amp>=er?1:0; er=er-amp+y*pk; word=(word<<1)|(uint32_t)y; ph+=inc; }
      out[i]=word; }
    phase=ph; err=er; updateFade();
  }
  String getIdentifier() override { return "B_fold"; }
private: int32_t fold=0;
};

class safe4FMMorphModel : public safe4BaseOscillatorModel {
public:
  void ctrl(const Q16_16 v) override { depth=static_cast<int32_t>((static_cast<int64_t>(v.raw())*72)>>16); }
  void fillBuffer(uint32_t* out) override {
    const uint32_t inc=(256U<<16)/static_cast<uint32_t>(wavelen); uint32_t ph=phase; int32_t er=err; const int32_t pk=peak(),d=depth;
    for(size_t i=0;i<loopLength;i++){ uint32_t word=0;
      for(size_t bit=0;bit<32;bit++){ uint32_t base=(ph>>16)&0xFF, mi=(ph>>15)&0xFF; int32_t mod=(int32_t)Safe4Tables::sine256[mi]-1024;
        uint32_t ci=(base+((mod*d)>>10))&0xFF; int32_t amp=Safe4Tables::sine256[ci],y=amp>=er?1:0;
        er=er-amp+y*pk; word=(word<<1)|(uint32_t)y; ph+=inc; }
      out[i]=word; }
    phase=ph; err=er; updateFade();
  }
  String getIdentifier() override { return "C_fm"; }
private: int32_t depth=0;
};

class safe4VowelMorphModel : public safe4BaseOscillatorModel {
public:
  void ctrl(const Q16_16 v) override {
    uint32_t scaled=(uint32_t)(((uint64_t)(uint32_t)v.raw()*4096U)>>16); if(scaled>4095U)scaled=4095U; seg=scaled>>10; morph=scaled&1023;
  }
  void fillBuffer(uint32_t* out) override {
    static const uint16_t* const tabs[5]={Safe4Tables::vowel0,Safe4Tables::vowel1,Safe4Tables::vowel2,Safe4Tables::vowel3,Safe4Tables::vowel4};
    const uint32_t inc=(256U<<16)/static_cast<uint32_t>(wavelen); uint32_t ph=phase; int32_t er=err; const int32_t pk=peak(),m=morph;
    const uint16_t* a=tabs[seg]; const uint16_t* b=tabs[seg+1];
    for(size_t i=0;i<loopLength;i++){ uint32_t word=0;
      for(size_t bit=0;bit<32;bit++){ uint32_t idx=(ph>>16)&0xFF; int32_t av=a[idx],bv=b[idx],amp=av+(((bv-av)*m)>>10),y=amp>=er?1:0;
        er=er-amp+y*pk; word=(word<<1)|(uint32_t)y; ph+=inc; }
      out[i]=word; }
    phase=ph; err=er; updateFade();
  }
  String getIdentifier() override { return "D_vowel"; }
private: uint32_t seg=0; int32_t morph=0;
};
'''.replace('__TABLES__',tables)
(ROOT/'arduino_libmyriad'/'oscmodels'/'safe4OscillatorModels.hpp').write_text(header)

osc=ROOT/'arduino_libmyriad'/'oscillatorModels.hpp'
s=osc.read_text(); assert 'N_OSCILLATOR_MODELS = 13;' in s; assert 'safe4OscillatorModels.hpp' not in s
s=s.replace('N_OSCILLATOR_MODELS = 13;','N_OSCILLATOR_MODELS = 17;')
s=s.replace('#include "oscmodels/triTeethOscillatorModel.hpp"','#include "oscmodels/triTeethOscillatorModel.hpp"\n#include "oscmodels/safe4OscillatorModels.hpp"')
osc.write_text(s)

b=ROOT/'Myriad_B'/'Myriad_B.ino'; s=b.read_text()
needle='static silentOscillatorModel        silentModels0[3],       silentModels1[3];'; assert needle in s
s=s.replace(needle,needle+'\nstatic safe4PureSineModel            safe4SineModels0[3],     safe4SineModels1[3];\nstatic safe4WavefolderModel          safe4FoldModels0[3],     safe4FoldModels1[3];\nstatic safe4FMMorphModel             safe4FMModels0[3],       safe4FMModels1[3];\nstatic safe4VowelMorphModel          safe4VowelModels0[3],    safe4VowelModels1[3];')
for core in ('0','1'):
    needle=f'    allOscModels{core}[12][i] = &silentModels{core}[i];'; assert needle in s
    s=s.replace(needle,needle+f'\n    allOscModels{core}[13][i] = &safe4SineModels{core}[i];\n    allOscModels{core}[14][i] = &safe4FoldModels{core}[i];\n    allOscModels{core}[15][i] = &safe4FMModels{core}[i];\n    allOscModels{core}[16][i] = &safe4VowelModels{core}[i];')
b.write_text(s)

a=ROOT/'Myriad_A'/'Myriad_A.ino'; s=a.read_text(); assert '#define MYRIAD_VERSION "1.1.2"' in s
s=s.replace('#define MYRIAD_VERSION "1.1.2"','#define MYRIAD_VERSION "1.1.2-SAFE4"')
needle='static silentOscillatorModel        silentModels[3];'; assert needle in s
s=s.replace(needle,needle+'\nstatic safe4PureSineModel            safe4SineModels[3];\nstatic safe4WavefolderModel          safe4FoldModels[3];\nstatic safe4FMMorphModel             safe4FMModels[3];\nstatic safe4VowelMorphModel          safe4VowelModels[3];')
needle='    allOscModels[12][i] = &silentModels[i];'; assert needle in s
s=s.replace(needle,needle+'\n    allOscModels[13][i] = &safe4SineModels[i];\n    allOscModels[14][i] = &safe4FoldModels[i];\n    allOscModels[15][i] = &safe4FMModels[i];\n    allOscModels[16][i] = &safe4VowelModels[i];')
state='''  oscBankTypes[0] = MyriadState::getOscBank(0);\n  oscBankTypes[1] = MyriadState::getOscBank(1);\n  oscBankTypes[2] = MyriadState::getOscBank(2);'''; assert state in s
s=s.replace(state,state+'\n  for(size_t i=0;i<3;i++) if(oscBankTypes[i]>=N_OSCILLATOR_MODELS) oscBankTypes[i]=0;')
s=s.replace('MyriadState::setOscBank(i, oscBankTypes[i]);','MyriadState::setOscBank(i, oscBankTypes[i] < 13 ? oscBankTypes[i] : 0);')
a.write_text(s)

assert 'N_OSCILLATOR_MODELS = 17;' in osc.read_text()
assert 'allOscModels0[16][i]' in b.read_text() and 'allOscModels1[16][i]' in b.read_text()
assert 'allOscModels[16][i]' in a.read_text()
assert 'oscBankTypes[i] < 13 ? oscBankTypes[i] : 0' in a.read_text()
print('SAFE4 source generation and 1.1.2 structural validation: PASS')
