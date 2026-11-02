#!/usr/bin/env python3
from pathlib import Path
import json, sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root = Path(sys.argv[1]); s=json.loads((root/'summary.json').read_text())
labels={'integer':'Integer mix','branch':'Data-dependent branches','stream':'Sequential array','pointer':'Pointer chase','float':'Float64 arithmetic'}
fig,axes=plt.subplots(1,2,figsize=(11,4.8),sharey=True)
for ax,key,title in zip(axes,['v86OverDynarmic','dynarmicOverWasm'],['v86 time / dynarmic.js time','dynarmic.js time / direct Wasm time']):
 for i,(case,v) in enumerate(s['cases'].items()):
  r=v['ratios'][key]
  ax.scatter(r['samples'],[i-.12,i-.06,i,i+.06,i+.12],color='#9aaebf',s=20,zorder=3)
  ax.errorbar(r['median'],i,xerr=[[r['median']-r['min']],[r['max']-r['median']]],fmt='D',markersize=5,color='#215e91',capsize=3,zorder=4)
 ax.axvline(1,ls='--',color='#777',lw=1)
 ax.set_xlabel(title);ax.grid(axis='x',color='#e5e7eb');ax.set_axisbelow(True)
 for side in ['top','right','left']:ax.spines[side].set_visible(False)
 ax.tick_params(axis='y',length=0)
axes[0].set_yticks(range(5),[labels[c] for c in s['cases']]);axes[0].invert_yaxis()
fig.suptitle('Same C source and Clang: browser execution paths',x=.03,ha='left',fontsize=15,weight='bold')
fig.text(.03,.91,'Core i7-12700 · Chromium 149 · O2 scalar · Extended workloads',fontsize=10,color='#4b5563')
fig.text(.03,.02,'Five fresh browser sessions. Dots are session medians; bars show observed min–max, not confidence intervals.',fontsize=9,color='#4b5563')
fig.subplots_adjust(left=.23,right=.98,top=.83,bottom=.17,wspace=.26)
fig.savefig(root/'comparison.png',dpi=160)
fig.savefig(root/'comparison.svg',metadata={'Date':None})
p=root/'comparison.svg';p.write_text('\n'.join(x.rstrip() for x in p.read_text().splitlines())+'\n')
