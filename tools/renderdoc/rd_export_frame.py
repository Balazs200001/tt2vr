"""
rd_export_frame.py - export a RenderDoc (v1.37) D3D11 capture to TSV + markdown for tt2vr Phase 0.

USAGE (qrenderdoc, with the .rdc OPEN and fully loaded - wait until the load bar is gone):
  Tools > Start Python Shell > "Run Scripts" tab > open this file > Run     (or in the Interactive tab:
  exec(open(r"C:\\dev\\tt2vr\\tools\\renderdoc\\rd_export_frame.py").read()) )
  Optional before running:  RD_MAX_EVENTS = 200   (debug: only process first N leaf actions)
OUTPUT (next to the capture, e.g. foo.rdc ->):
  foo.rdc.actions.tsv  one row per action (markers included, pipeline columns only for leaf actions)
  foo.rdc.passes.md    pass summary, Clear events (+values), cmd-list/deferred events, unique shaders
Python 3.6 compatible, no external modules. Every access is wrapped in try/except; failures leave "?" cells
and are counted at the end of passes.md. NOT RUN YET against a real capture (written from the 1.37 docs):
if a column is all "?", check the ERRORS section of passes.md and fix the matching API call.
Shader hashes: xxh64(seed 0) of ShaderReflection.rawBytes (match runtime registry, if it hashes the raw DXBC blob),
  sha1, byte size, and the DXBC header MD5 (bytes 4..20 of the blob; stable id, readable in the game too).
"""
import os, hashlib, collections, traceback
import renderdoc as rd

MAXEV = globals().get("RD_MAX_EVENTS", 0)
ERR = collections.Counter()
ERR_EX = {}


def guard(tag):
    ERR[tag] += 1
    if tag not in ERR_EX:
        ERR_EX[tag] = traceback.format_exc().strip().splitlines()[-1]


# ---------------------------------------------------------------- xxh64 (pure python, seed 0)
_M = 0xFFFFFFFFFFFFFFFF
_P1, _P2, _P3, _P4, _P5 = (11400714785074694791, 14029467366897019727, 1609587929392839161,
                            9650029242287828579, 2870177450012600261)


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & _M


def _rnd(acc, v):
    return (_rotl((acc + v * _P2) & _M, 31) * _P1) & _M


def _mrg(acc, v):
    return (((acc ^ _rnd(0, v)) * _P1) + _P4) & _M


def xxh64(data):
    n = len(data)
    i = 0
    rd8 = lambda o: int.from_bytes(data[o:o + 8], "little")
    rd4 = lambda o: int.from_bytes(data[o:o + 4], "little")
    if n >= 32:
        v = [(_P1 + _P2) & _M, _P2, 0, (-_P1) & _M]
        while i <= n - 32:
            for k in range(4):
                v[k] = _rnd(v[k], rd8(i + 8 * k))
            i += 32
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & _M
        for k in range(4):
            h = _mrg(h, v[k])
    else:
        h = _P5
    h = (h + n) & _M
    while i + 8 <= n:
        h ^= _rnd(0, rd8(i))
        h = (_rotl(h, 27) * _P1 + _P4) & _M
        i += 8
    if i + 4 <= n:
        h ^= (rd4(i) * _P1) & _M
        h = (_rotl(h, 23) * _P2 + _P3) & _M
        i += 4
    while i < n:
        h ^= (data[i] * _P5) & _M
        h = (_rotl(h, 11) * _P1) & _M
        i += 1
    h ^= h >> 33
    h = (h * _P2) & _M
    h ^= h >> 29
    h = (h * _P3) & _M
    h ^= h >> 32
    return h


# ---------------------------------------------------------------- helpers
def clean(s):
    return str(s).replace("\t", " ").replace("\r", " ").replace("\n", " ")


def enum_name(e):
    s = str(e)
    return s.split(".")[-1]


def flag_names(flags):
    out = []
    for n in dir(rd.ActionFlags):
        if n.startswith("_") or n == "NoFlags":
            continue
        try:
            v = getattr(rd.ActionFlags, n)
            if int(flags) & int(v):
                out.append(n)
        except Exception:
            pass
    return out


def fmt_name(f):
    try:
        return f.Name()
    except Exception:
        return "?"


class Ctx(object):
    pass


def tex_desc(C, rid):
    """'name[fmt WxH(xD)(mips)]' for a texture/buffer ResourceId."""
    try:
        if rid == rd.ResourceId.Null():
            return "-"
    except Exception:
        pass
    nm = C.names.get(rid, str(rid))
    t = C.tex.get(rid)
    if t is not None:
        d = "x%d" % t.depth if t.depth > 1 else ""
        a = "[%d]" % t.arraysize if t.arraysize > 1 else ""
        return "%s{%s %dx%d%s%s m%d}" % (nm, fmt_name(t.format), t.width, t.height, d, a, t.mips)
    b = C.buf.get(rid)
    if b is not None:
        return "%s{buf %dB}" % (nm, b.length)
    return nm


def desc_str(C, d, tag_fmt=True):
    try:
        s = tex_desc(C, d.resource)
        f = fmt_name(d.format)
        if tag_fmt and f not in ("?", "Unknown", "") and s != "-":
            s += "(view %s)" % f
        return s
    except Exception:
        guard("desc_str")
        return "?"


def rid_key(rid):
    return str(rid)


def shader_info(C, rid, refl):
    """returns (idstr, size, sha1, xxh64hex, dxbcmd5). Cached per ResourceId."""
    key = rid_key(rid)
    if key in C.shcache:
        return C.shcache[key]
    res = (key, 0, "?", "?", "?")
    try:
        raw = bytes(refl.rawBytes)
        md5 = raw[4:20].hex() if raw[:4] == b"DXBC" else "-"
        res = (key, len(raw), hashlib.sha1(raw).hexdigest()[:16], "%016x" % xxh64(raw), md5)
    except Exception:
        guard("shader_hash")
    C.shcache[key] = res
    return res


def stage_binding_str(C, pipe, stage, refl):
    """(srv_str, uav_str, cb_str) for a shader stage."""
    srv, uav, cbs = [], [], []
    try:
        for ud in pipe.GetReadOnlyResources(stage, True):
            try:
                idx = ud.access.index
                slot, nm = "?", "?"
                if refl is not None and idx < len(refl.readOnlyResources):
                    r = refl.readOnlyResources[idx]
                    slot, nm = r.fixedBindNumber, r.name
                srv.append("t%s:%s=%s" % (slot, nm, desc_str(C, ud.descriptor)))
            except Exception:
                guard("srv")
    except Exception:
        guard("GetReadOnlyResources")
    try:
        for ud in pipe.GetReadWriteResources(stage, True):
            try:
                idx = ud.access.index
                slot, nm = "?", "?"
                if refl is not None and idx < len(refl.readWriteResources):
                    r = refl.readWriteResources[idx]
                    slot, nm = r.fixedBindNumber, r.name
                uav.append("u%s:%s=%s" % (slot, nm, desc_str(C, ud.descriptor)))
            except Exception:
                guard("uav")
    except Exception:
        guard("GetReadWriteResources")
    try:
        if refl is not None:
            for cb in refl.constantBlocks:
                cbs.append("b%s:%s" % (cb.fixedBindNumber, cb.name))
    except Exception:
        guard("cbs")
    return ";".join(srv), ";".join(uav), ";".join(cbs)


def sdobj_str(o, depth=0):
    try:
        if o.NumChildren() == 0 or depth >= 2:
            return "%s=%s" % (o.name, clean(o.AsString()))
        return "%s{%s}" % (o.name, ",".join(sdobj_str(o.GetChild(i), depth + 1) for i in range(o.NumChildren())))
    except Exception:
        guard("sdobj")
        return "?"


def event_chunks(C, a):
    """list of (chunkName, paramString) for events attached to action a."""
    out = []
    try:
        for ev in a.events:
            try:
                ch = C.sf.chunks[ev.chunkIndex]
                params = ",".join(sdobj_str(ch.GetChild(i)) for i in range(ch.NumChildren()))
                out.append((ch.name, params))
            except Exception:
                guard("chunk")
    except Exception:
        guard("events")
    return out


DEPTH_ENUM_KEYS = ("depthFunction", "function")


def depth_state(controller, pipe):
    """'func/en/wr' string."""
    try:
        st = controller.GetD3D11PipelineState()
        ds = st.outputMerger.depthStencilState
        return "%s/%s/%s" % (enum_name(ds.depthFunction), "Z" if ds.depthEnable else "noZ",
                             "W" if ds.depthWrites else "noW")
    except Exception:
        guard("d3d11_depth")
    try:
        d = pipe.GetDepthTestState()
        f = getattr(d, "function", getattr(d, "depthFunction", "?"))
        return "%s/%s/%s" % (enum_name(f), getattr(d, "enabled", "?"), getattr(d, "writeEnable", "?"))
    except Exception:
        guard("depth_generic")
    return "?"


STAGES = [("VS", rd.ShaderStage.Vertex), ("HS", rd.ShaderStage.Hull), ("DS", rd.ShaderStage.Domain),
          ("GS", rd.ShaderStage.Geometry), ("PS", rd.ShaderStage.Pixel), ("CS", rd.ShaderStage.Compute)]

COLS = ["eid", "depth_in_marker", "marker_path", "name", "flags", "idx_or_dispatch", "instances",
        "rtv0..7", "dsv", "depth_func", "viewport0", "VS", "PS", "CS", "HS", "DS", "GS",
        "PS_srv", "PS_cbs", "VS_srv", "CS_srv", "CS_uav", "PS_uav", "VS_cbs", "CS_cbs", "copy_src>dst", "chunks"]

PASSBREAK_FLAGS = ("Drawcall", "Dispatch", "Clear", "Copy", "Resolve", "GenMips")


def fill_pipeline(C, controller, a, flags, row):
    """fills row dict with pipeline-derived columns; returns pass key (tuple)."""
    controller.SetFrameEvent(a.eventId, True)
    pipe = controller.GetPipelineState()
    rts, rtkey = [], []
    try:
        for d in pipe.GetOutputTargets():
            if d.resource != rd.ResourceId.Null():
                rts.append(desc_str(C, d))
                rtkey.append(rid_key(d.resource))
    except Exception:
        guard("GetOutputTargets")
    dsv, dskey = "-", ""
    try:
        d = pipe.GetDepthTarget()
        if d.resource != rd.ResourceId.Null():
            dsv = desc_str(C, d)
            dskey = rid_key(d.resource)
    except Exception:
        guard("GetDepthTarget")
    is_draw = "Drawcall" in flags
    row["rtv0..7"] = " | ".join(rts) if rts else "-"
    row["dsv"] = dsv
    if is_draw:
        row["depth_func"] = depth_state(controller, pipe)
        try:
            v = pipe.GetViewport(0)
            row["viewport0"] = "%g,%g %gx%g z[%g,%g]" % (v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth)
        except Exception:
            guard("GetViewport")
    wanted = ("VS", "PS") if is_draw else ("CS",) if "Dispatch" in flags else ()
    for nm, st in STAGES:
        try:
            sid = pipe.GetShader(st)
            if sid == rd.ResourceId.Null():
                continue
            refl = pipe.GetShaderReflection(st)
            info = shader_info(C, sid, refl)
            row[nm] = "%s sz%d x64:%s md5:%s sha1:%s" % (info[0], info[1], info[3], info[4], info[2])
            C.shuse[(nm, info)] = C.shuse.get((nm, info), 0) + 1
            C.shfirst.setdefault((nm, info), a.eventId)
            if nm in wanted or nm in ("HS", "DS", "GS"):
                s, u, c = stage_binding_str(C, pipe, st, refl)
                if nm in ("PS", "VS", "CS"):
                    row[nm + "_srv"] = s
                    row[nm + "_cbs"] = c
                if nm in ("CS", "PS"):
                    row[nm + "_uav"] = u
        except Exception:
            guard("stage_" + nm)
    if "Dispatch" in flags and not rts:
        return ("CS",)
    return (tuple(rtkey), dskey)


def walk(C, controller, acts, path, depth, rows):
    for a in acts:
        if MAXEV and C.leafcount >= MAXEV:
            return
        row = {}
        try:
            fl = flag_names(a.flags)
            try:
                nm = a.GetName(C.sf)
            except Exception:
                nm = a.customName
            row["eid"] = a.eventId
            row["depth_in_marker"] = depth
            row["marker_path"] = "/".join(path)
            row["name"] = clean(nm)
            row["flags"] = ",".join(fl)
            if "Drawcall" in fl:
                row["idx_or_dispatch"] = a.numIndices
                row["instances"] = a.numInstances
            elif "Dispatch" in fl:
                row["idx_or_dispatch"] = "x".join(str(x) for x in a.dispatchDimension)
            leaf = any(f in fl for f in PASSBREAK_FLAGS)
            if leaf:
                C.leafcount += 1
                key = fill_pipeline(C, controller, a, fl, row)
                C.passlist.append((a.eventId, "/".join(path), row.get("name", ""), key, fl, row))
            if "Copy" in fl or "Resolve" in fl or "GenMips" in fl:
                row["copy_src>dst"] = "%s>%s" % (tex_desc(C, a.copySource), tex_desc(C, a.copyDestination))
            if leaf or "CmdList" in fl or "Clear" in fl:
                ch = event_chunks(C, a)
                row["chunks"] = " || ".join("%s(%s)" % (n, p) for n, p in ch)[:600]
                if "Clear" in fl:
                    C.clears.append((a.eventId, "/".join(path), row["name"], fl, row.get("rtv0..7", "-"),
                                     row.get("dsv", "-"), ch))
                if "CmdList" in fl or any("CommandList" in n or "Deferred" in n for n, _ in ch):
                    C.cmdlists.append((a.eventId, "/".join(path), row["name"], fl, ch))
            if "Present" in fl:
                C.presents.append((a.eventId, row["name"]))
        except Exception:
            guard("walk_action")
        rows.append(row)
        if a.children:
            walk(C, controller, a.children, path + [clean(row.get("name", "?"))], depth + 1, rows)


def write_outputs(C, rows, base):
    with open(base + ".actions.tsv", "w", encoding="utf-8") as f:
        f.write("\t".join(COLS) + "\n")
        for r in rows:
            f.write("\t".join(clean(r.get(c, "")) for c in COLS) + "\n")
    md = []
    md.append("# Passes: %s\n" % os.path.basename(base))
    md.append("Actions: %d rows, %d leaf (draw/dispatch/clear/copy). Presents at EIDs: %s\n" %
              (len(rows), C.leafcount, ", ".join(str(e) for e, _ in C.presents) or "none"))
    # passes = consecutive leaf actions with same key (rt set + dsv, or CS)
    md.append("\n## Passes (consecutive leaf actions with the same RT/DSV set; Clear/Copy break nothing by themselves)\n")
    md.append("| EID range | marker path (first) | RT set | DSV | draws | disp | clears | copies |")
    md.append("|---|---|---|---|---|---|---|---|")
    cur = None

    def flush(p):
        if p is None:
            return
        rt = p["rt"]
        md.append("| %d-%d | %s | %s | %s | %d | %d | %d | %d |" % (p["a"], p["b"], p["path"][-120:],
                                                              rt, p["dsv"], p["dr"], p["di"], p["cl"], p["cp"]))
    for eid, path, name, key, fl, row in C.passlist:
        gfx = "Drawcall" in fl or "Clear" in fl
        if "Copy" in fl or "Resolve" in fl or "GenMips" in fl:
            if cur is not None:
                cur["cp"] += 1
            else:
                cur = {"a": eid, "b": eid, "path": path, "rt": "(copy)", "dsv": "-", "dr": 0, "di": 0, "cl": 0,
                       "cp": 1, "key": ("copy",)}
            cur["b"] = eid
            continue
        if cur is None or cur["key"] != key:
            if cur is not None:
                flush(cur)
            cur = {"a": eid, "b": eid, "path": path, "key": key, "rt": row.get("rtv0..7", "-") if "Dispatch" not in fl else "(compute)",
                   "dsv": row.get("dsv", "-"), "dr": 0, "di": 0, "cl": 0, "cp": 0}
        cur["b"] = eid
        if "Drawcall" in fl:
            cur["dr"] += 1
        if "Dispatch" in fl:
            cur["di"] += 1
        if "Clear" in fl:
            cur["cl"] += 1
    flush(cur)
    md.append("\n## Clear events (depth clear value 0 => reversed-Z, 1 => standard; check also depth_func GreaterEqual/Greater in tsv)\n")
    md.append("| EID | marker | name | target | params |")
    md.append("|---|---|---|---|---|")
    for eid, path, name, fl, rt, dsv, ch in C.clears:
        md.append("| %d | %s | %s | %s | %s |" % (eid, path[-100:], name, (dsv if dsv != "-" else rt)[:80],
                                                   clean(" || ".join(p for _, p in ch))[:200]))
    md.append("\n## ExecuteCommandList / deferred-context events\n")
    if not C.cmdlists:
        md.append("(none found: either no deferred contexts in this frame or 'Capture All Cmd Lists' was off)")
    md.append("| EID | marker | name | flags | chunks |")
    md.append("|---|---|---|---|---|")
    for eid, path, name, fl, ch in C.cmdlists:
        md.append("| %d | %s | %s | %s | %s |" % (eid, path[-100:], name, ",".join(fl),
                                                  clean(" || ".join("%s(%s)" % x for x in ch))[:200]))
    md.append("\n## Unique shaders (stage, resourceId, size, xxh64 of rawBytes, DXBC-header md5, sha1[:16], first EID, uses)\n")
    md.append("| stage | id | bytes | xxh64 | dxbc md5 | sha1 | first EID | uses |")
    md.append("|---|---|---|---|---|---|---|---|")
    for (nm, info), n in sorted(C.shuse.items(), key=lambda kv: C.shfirst[kv[0]]):
        md.append("| %s | %s | %d | %s | %s | %s | %d | %d |" % (nm, info[0], info[1], info[3], info[4], info[2],
                                                               C.shfirst[(nm, info)], n))
    md.append("\n## ERRORS (API calls that raised; counts)\n")
    for k, v in ERR.most_common():
        md.append("- %s: %d (last: %s)" % (k, v, ERR_EX.get(k, "")))
    if not ERR:
        md.append("(none)")
    with open(base + ".passes.md", "w", encoding="utf-8") as f:
        f.write("\n".join(md) + "\n")


def replay_cb(controller):
    C = Ctx()
    C.shcache, C.shuse, C.shfirst = {}, {}, {}
    C.passlist, C.clears, C.cmdlists, C.presents, C.leafcount = [], [], [], [], 0
    C.names, C.tex, C.buf = {}, {}, {}
    try:
        for r in controller.GetResources():
            C.names[r.resourceId] = r.name
        for t in controller.GetTextures():
            C.tex[t.resourceId] = t
        for b in controller.GetBuffers():
            C.buf[b.resourceId] = b
    except Exception:
        guard("resources")
    try:
        C.sf = controller.GetStructuredFile()
    except Exception:
        guard("GetStructuredFile")
        C.sf = pyrenderdoc.GetStructuredFile() if hasattr(pyrenderdoc, "GetStructuredFile") else None
    rows = []
    walk(C, controller, controller.GetRootActions(), [], 0, rows)
    base = pyrenderdoc.GetCaptureFilename()
    write_outputs(C, rows, base)
    print("rd_export_frame: wrote %s.actions.tsv (%d rows) and .passes.md; errors: %s" %
          (base, len(rows), dict(ERR) or "none"))


try:
    pyrenderdoc.Replay().BlockInvoke(replay_cb)
except NameError:
    print("pyrenderdoc not defined: run this inside qrenderdoc's Python shell with a capture loaded.")
