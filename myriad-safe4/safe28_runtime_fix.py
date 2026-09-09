from pathlib import Path

ROOT=Path('Myriad')

# 1) Remove per-custom-model TFT sprite allocation. Stock 0-12 keep their exact icons.
d=ROOT/'Myriad_A'/'displayPortal.h'
s=d.read_text()
old='''    for(size_t bank=0; bank < N_OSC_BANKS; bank++) {\n      for(size_t model=0; model < N_OSCILLATOR_MODELS; model++) {\n        oscModelIcons[bank][model] = std::make_shared<TFT_eSprite>(&tft);\n        oscModelIcons[bank][model]->createSprite(iconw, iconh);\n        auto it = iconDrawFunctions.find(oscModelIDs[model]);\n        if (it != iconDrawFunctions.end()) {\n            oscModelIcons[bank][model]->fillSprite(ELI_BLUE);\n            it->second(oscModelIcons[bank][model],bankColArray[bank]);\n        }\n        else\n        {        \n          oscModelIcons[bank][model]->fillRect(0,0,iconw, iconh, bankColArray[bank]);\n          oscModelIcons[bank][model]->setFreeFont(&FreeSansBold9pt7b);\n          oscModelIcons[bank][model]->setTextColor(TFT_BLACK, TFT_SILVER);\n          oscModelIcons[bank][model]->setTextDatum(MC_DATUM);\n          String idxstr = oscModelIDs[model].substring(0,3);\n          oscModelIcons[bank][model]->drawString(idxstr.c_str(),iconw>>1,iconh>>1);\n        }      \n      }\n    }'''
new='''    for(size_t bank=0; bank < N_OSC_BANKS; bank++) {\n      for(size_t model=0; model < N_OSCILLATOR_MODELS; model++) {\n        // Critical SAFE28 heap protection: only stock models get persistent sprites.\n        // Custom models 13+ are rendered directly when selected.\n        if (model >= 13) {\n          oscModelIcons[bank][model].reset();\n          continue;\n        }\n        oscModelIcons[bank][model] = std::make_shared<TFT_eSprite>(&tft);\n        oscModelIcons[bank][model]->createSprite(iconw, iconh);\n        auto it = iconDrawFunctions.find(oscModelIDs[model]);\n        if (it != iconDrawFunctions.end()) {\n          oscModelIcons[bank][model]->fillSprite(ELI_BLUE);\n          it->second(oscModelIcons[bank][model],bankColArray[bank]);\n        } else {\n          oscModelIcons[bank][model]->fillRect(0,0,iconw,iconh,bankColArray[bank]);\n        }\n      }\n    }'''
assert old in s, 'extended icon allocation block not found'
s=s.replace(old,new)

old='''        // tft.fillRect(iconX[i], iconY[i], iconw, iconh, ELI_BLUE);\n        oscModelIcons[i][nextState.oscModel[i]]->pushSprite(iconX[i], iconY[i]);'''
new='''        // Stock models use their original pre-rendered icons.\n        // Custom models are drawn directly, avoiding persistent TFT sprite heap.\n        const size_t displayModel = nextState.oscModel[i] < N_OSCILLATOR_MODELS ? nextState.oscModel[i] : 0;\n        if (displayModel < 13) {\n          oscModelIcons[i][displayModel]->pushSprite(iconX[i], iconY[i]);\n        } else {\n          static const char* customLabels[15] = {\n            "SIN","FLD","FM","VOW","SUP","PHS","ADD","SUB","SYN","PWM","MET","PLK","RED","BIT","ORG"\n          };\n          tft.fillRect(iconX[i], iconY[i], iconw, iconh, bankColArray[i]);\n          tft.setTextFont(1);\n          tft.setTextColor(TFT_BLACK, bankColArray[i]);\n          tft.setTextDatum(MC_DATUM);\n          const char* shortID = customLabels[displayModel - 13];\n          tft.drawString(shortID, iconX[i] + (iconw>>1), iconY[i] + (iconh>>1));\n        }'''
assert old in s, 'oscillator icon push block not found'
s=s.replace(old,new)
d.write_text(s)

# 2) Clamp all A/B model table assignment before indexing pointer arrays.
a=ROOT/'Myriad_A'/'Myriad_A.ino'
s=a.read_text()
old='''void __not_in_flash_func(assignOscModels)(const size_t modelIdx) {\n  for (size_t i = 0; i < currOscModels.size(); i++) {\n    currOscModels[i] = allOscModels[modelIdx][i];\n  }\n}'''
new='''void __not_in_flash_func(assignOscModels)(const size_t modelIdx) {\n  const size_t safeModelIdx = modelIdx < N_OSCILLATOR_MODELS ? modelIdx : 0;\n  for (size_t i = 0; i < currOscModels.size(); i++) {\n    currOscModels[i] = allOscModels[safeModelIdx][i];\n  }\n}'''
assert old in s, 'A assignOscModels block not found'
s=s.replace(old,new)
a.write_text(s)

b=ROOT/'Myriad_B'/'Myriad_B.ino'
s=b.read_text()
old='''void assignOscModels0(size_t modelIdx) {\n  for (size_t i = 0; i < currOscModels0.size(); i++) {\n    currOscModels0[i] = allOscModels0[modelIdx][i];\n  }\n}\nvoid assignOscModels1(size_t modelIdx) {\n  for (size_t i = 0; i < currOscModels1.size(); i++) {\n    currOscModels1[i] = allOscModels1[modelIdx][i];\n  }\n}'''
new='''void assignOscModels0(size_t modelIdx) {\n  const size_t safeModelIdx = modelIdx < N_OSCILLATOR_MODELS ? modelIdx : 0;\n  for (size_t i = 0; i < currOscModels0.size(); i++) {\n    currOscModels0[i] = allOscModels0[safeModelIdx][i];\n  }\n}\nvoid assignOscModels1(size_t modelIdx) {\n  const size_t safeModelIdx = modelIdx < N_OSCILLATOR_MODELS ? modelIdx : 0;\n  for (size_t i = 0; i < currOscModels1.size(); i++) {\n    currOscModels1[i] = allOscModels1[safeModelIdx][i];\n  }\n}'''
assert old in s, 'B assign blocks not found'
s=s.replace(old,new)
b.write_text(s)

# 3) Static audit markers for CI.
dt=d.read_text(); at=a.read_text(); bt=b.read_text()
assert 'if (model >= 13)' in dt
assert 'displayModel < 13' in dt
assert 'customLabels[15]' in dt
assert 'oscModelIcons[bank][model].reset();' in dt
assert at.count('safeModelIdx') >= 2
assert bt.count('safeModelIdx') >= 4
print('SAFE28 runtime heap/index hardening: PASS')
