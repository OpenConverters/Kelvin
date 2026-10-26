"""Usage: coverage_audit.py OUT.json TAS_DATA_DIR SCRATCH_INDEX_DIR"""
import sys,json
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build'))
import PyKelvin as k
e=k.Engine(sys.argv[2],sys.argv[3],False)
out={}
for fam in k.family_names():
    p=e.browse(fam,{"limit":0,"withFacets":True,"facetTop":3})
    n=p["rowCount"]; fields=k.family_fields(fam)
    rng=p.get("ranges") or {}
    cov={f:round(100*(rng.get(f,{}).get("present",0))/n,1) for f in fields["numeric"]}
    cat={}
    for f,v in (p.get("facets") or {}).items():
        notst=sum(c for val,c in v.get("values",[]) if not val)
        cat[f]=round(100*(1-notst/n),1) if v.get("values") else None
    out[fam]={"rows":n,"numeric_pct_stated":cov,"categorical_pct_stated":cat}
json.dump(out,open(sys.argv[1],'w'),indent=1)
for fam,v in out.items():
    print(fam,v["rows"]); 
    for f,c in sorted(v["numeric_pct_stated"].items(),key=lambda x:x[1])[:6]: print("   num",f,c)
    for f,c in sorted(v["categorical_pct_stated"].items(),key=lambda x:(x[1] is None,x[1]))[:4]: print("   cat",f,c)
