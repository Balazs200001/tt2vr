// TT2QueryServer.java - read-only headless Ghidra query server for tt2vr reverse engineering.
//
// One Ghidra process keeps the analyzed program open and answers queries from any number of clients
// through files, so several agents/tools can query it at the same time without project-lock conflicts.
//
// Launch (see start-query-server.ps1):
//   analyzeHeadless <projDir> <projName> -process TT2.exe -noanalysis -readOnly
//                   -scriptPath <this dir> -postScript TT2QueryServer.java <workdir>
// Protocol: client writes <workdir>/inbox/<id>.q (one command line, UTF-8);
//           server answers in <workdir>/outbox/<id>.out. Create <workdir>/stop to shut down.
// Commands (T = 0x-address, or a symbol name: mangled, or Namespace::Class::Method):
//   ping | func T | decompile T | disasm T [maxInsns] | xrefs T | callers T | callees T
//   strings REGEX | symbols REGEX | bytes ADDR [n] | qwords ADDR [n]
// On first start it also writes bulk exports to <workdir>/exports/ (functions, string xrefs, import xrefs,
// call graph, vftables). Nothing is written to the program (-readOnly).
//@category tt2vr

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.symbol.ExternalLocation;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.symbol.SymbolType;
import ghidra.program.util.DefinedStringIterator;
import ghidra.util.task.TaskMonitor;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.regex.Pattern;

public class TT2QueryServer extends GhidraScript {

	private static final int WORKERS = 6;
	private static final int DECOMPILE_TIMEOUT_S = 120;
	private static final long IDLE_LIMIT_MS = 4L * 3600 * 1000;

	private File work, inbox, outbox, exportsDir;
	private PrintWriter logw;
	private final ThreadLocal<DecompInterface> decomp = new ThreadLocal<>();
	private final List<DecompInterface> allDecomps = Collections.synchronizedList(new ArrayList<>());
	private volatile long lastActivity = System.currentTimeMillis();

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			println("usage: TT2QueryServer <workdir>");
			return;
		}
		work = new File(args[0]);
		inbox = new File(work, "inbox");
		outbox = new File(work, "outbox");
		exportsDir = new File(work, "exports");
		inbox.mkdirs();
		outbox.mkdirs();
		exportsDir.mkdirs();
		new File(work, "stop").delete();
		logw = new PrintWriter(new OutputStreamWriter(
			new FileOutputStream(new File(work, "server.log"), true), StandardCharsets.UTF_8), true);
		log("server start program=" + currentProgram.getName() + " sha256=" +
			currentProgram.getExecutableSHA256());

		ExecutorService pool = Executors.newFixedThreadPool(WORKERS);
		if (!new File(exportsDir, "DONE").exists()) {
			Thread exporter = new Thread(this::bulkExport, "bulk-export");
			exporter.setDaemon(true);
			exporter.start();
		}
		write(new File(work, "ready"), "pid=" + ProcessHandle.current().pid() + "\nsha256=" +
			currentProgram.getExecutableSHA256() + "\n");
		try {
			while (!monitor.isCancelled()) {
				if (new File(work, "stop").exists()) {
					log("stop file seen");
					break;
				}
				if (System.currentTimeMillis() - lastActivity > IDLE_LIMIT_MS) {
					log("idle timeout");
					break;
				}
				File[] qs = inbox.listFiles((d, n) -> n.endsWith(".q"));
				if (qs != null) {
					Arrays.sort(qs);
					for (File q : qs) {
						File claimed = new File(inbox, q.getName() + ".work");
						if (!q.renameTo(claimed)) {
							continue;
						}
						lastActivity = System.currentTimeMillis();
						pool.submit(() -> handle(claimed));
					}
				}
				Thread.sleep(100);
			}
		}
		finally {
			pool.shutdown();
			pool.awaitTermination(180, TimeUnit.SECONDS);
			for (DecompInterface d : allDecomps) {
				try {
					d.dispose();
				}
				catch (Exception e) {
					// ignore
				}
			}
			new File(work, "ready").delete();
			log("server stop");
			logw.close();
		}
	}

	// ---------------------------------------------------------------- protocol

	private void handle(File claimed) {
		String id = claimed.getName().replaceAll("\\.q\\.work$", "");
		long t0 = System.currentTimeMillis();
		String cmdLine = "";
		String out;
		try {
			cmdLine = new String(Files.readAllBytes(claimed.toPath()), StandardCharsets.UTF_8).trim();
			out = dispatch(cmdLine);
		}
		catch (Throwable e) {
			StringWriter sw = new StringWriter();
			e.printStackTrace(new PrintWriter(sw));
			out = "ERROR " + e + "\n" + sw;
		}
		try {
			File tmp = new File(outbox, id + ".out.tmp");
			write(tmp, out);
			Files.move(tmp.toPath(), new File(outbox, id + ".out").toPath(),
				StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
		}
		catch (IOException e) {
			log("write failed " + id + " " + e);
		}
		claimed.delete();
		lastActivity = System.currentTimeMillis();
		log(String.format("%s %dms %s", id, System.currentTimeMillis() - t0,
			cmdLine.length() > 200 ? cmdLine.substring(0, 200) : cmdLine));
	}

	private String dispatch(String line) throws Exception {
		String[] p = line.split("\\s+", 2);
		String cmd = p[0].toLowerCase(Locale.ROOT);
		String arg = p.length > 1 ? p[1].trim() : "";
		String[] a = arg.isEmpty() ? new String[0] : arg.split("\\s+");
		switch (cmd) {
			case "ping":
				return "pong " + currentProgram.getExecutableSHA256();
			case "func":
				return funcInfo(arg);
			case "decompile":
				return decompile(arg);
			case "disasm":
				return disasm(a[0], a.length > 1 ? Integer.parseInt(a[1]) : 600);
			case "xrefs":
				return xrefs(arg);
			case "callers":
				return callers(arg);
			case "callees":
				return callees(arg);
			case "strings":
				return strings(arg);
			case "symbols":
				return symbols(arg);
			case "bytes":
				return bytes(a[0], a.length > 1 ? Integer.parseInt(a[1]) : 64);
			case "qwords":
				return qwords(a[0], a.length > 1 ? Integer.parseInt(a[1]) : 16);
			default:
				return "ERROR unknown command: " + cmd;
		}
	}

	// ---------------------------------------------------------------- resolution

	private static class Resolved {
		Address addr;
		Function fn;
		String note = "";
	}

	private Resolved resolve(String t) {
		Resolved r = new Resolved();
		t = t.trim();
		FunctionManager fm = currentProgram.getFunctionManager();
		if (t.matches("(?i)0x[0-9a-f]+")) {
			r.addr = currentProgram.getAddressFactory().getDefaultAddressSpace()
					.getAddress(Long.parseUnsignedLong(t.substring(2), 16));
			r.fn = fm.getFunctionContaining(r.addr);
			return r;
		}
		List<Symbol> syms = new ArrayList<>();
		try {
			syms.addAll(NamespaceUtils.getSymbols(t, currentProgram));
		}
		catch (Exception e) {
			// not a namespace path
		}
		SymbolIterator it = currentProgram.getSymbolTable().getSymbols(t);
		while (it.hasNext()) {
			Symbol s = it.next();
			if (!syms.contains(s)) {
				syms.add(s);
			}
		}
		if (syms.isEmpty()) {
			r.note = "unresolved: " + t + "\n";
			return r;
		}
		Symbol best = null;
		for (Symbol s : syms) {
			if (s.getSymbolType() == SymbolType.FUNCTION) {
				best = s;
				break;
			}
		}
		if (best == null) {
			best = syms.get(0);
		}
		r.addr = best.getAddress();
		r.fn = fm.getFunctionContaining(r.addr);
		if (syms.size() > 1) {
			StringBuilder sb = new StringBuilder("note: " + syms.size() + " symbols matched; using " +
				best.getAddress() + "\n");
			for (Symbol s : syms.subList(0, Math.min(12, syms.size()))) {
				sb.append("  ").append(s.getAddress()).append(' ').append(s.getName(true)).append('\n');
			}
			r.note = sb.toString();
		}
		return r;
	}

	private String fname(Function f) {
		return f == null ? "-" : f.getEntryPoint() + "\t" + f.getSymbol().getName(true);
	}

	// ---------------------------------------------------------------- commands

	private String funcInfo(String t) {
		Resolved r = resolve(t);
		if (r.fn == null) {
			return "ERROR no function for " + t + (r.addr != null ? " at " + r.addr : "") + "\n" + r.note;
		}
		Function f = r.fn;
		StringBuilder sb = new StringBuilder(r.note);
		sb.append("entry     ").append(f.getEntryPoint()).append('\n');
		sb.append("name      ").append(f.getSymbol().getName(true)).append('\n');
		sb.append("prototype ").append(f.getPrototypeString(false, true)).append('\n');
		sb.append("size      ").append(f.getBody().getNumAddresses()).append('\n');
		if (f.isThunk()) {
			sb.append("thunk ->  ").append(fname(f.getThunkedFunction(true))).append('\n');
		}
		sb.append("callers   ").append(f.getCallingFunctions(TaskMonitor.DUMMY).size()).append('\n');
		sb.append("callees   ").append(f.getCalledFunctions(TaskMonitor.DUMMY).size()).append('\n');
		Symbol[] all = currentProgram.getSymbolTable().getSymbols(f.getEntryPoint());
		for (Symbol s : all) {
			sb.append("label     ").append(s.getName(true)).append(s.isPrimary() ? " (primary)" : "").append('\n');
		}
		return sb.toString();
	}

	private DecompInterface dec() {
		DecompInterface d = decomp.get();
		if (d == null) {
			d = new DecompInterface();
			DecompileOptions o = new DecompileOptions();
			o.grabFromProgram(currentProgram);
			d.setOptions(o);
			d.toggleCCode(true);
			d.openProgram(currentProgram);
			decomp.set(d);
			allDecomps.add(d);
		}
		return d;
	}

	private String decompile(String t) {
		Resolved r = resolve(t);
		if (r.fn == null) {
			return "ERROR no function for " + t + (r.addr != null ? " at " + r.addr : "") + "\n" + r.note;
		}
		DecompileResults res = dec().decompileFunction(r.fn, DECOMPILE_TIMEOUT_S, TaskMonitor.DUMMY);
		StringBuilder sb = new StringBuilder(r.note);
		sb.append("// ").append(r.fn.getEntryPoint()).append(' ').append(r.fn.getSymbol().getName(true)).append('\n');
		if (res == null || !res.decompileCompleted() || res.getDecompiledFunction() == null) {
			sb.append("ERROR decompile failed: ").append(res == null ? "null" : res.getErrorMessage());
			return sb.toString();
		}
		sb.append(res.getDecompiledFunction().getC());
		return sb.toString();
	}

	private String disasm(String t, int max) {
		Resolved r = resolve(t);
		if (r.addr == null) {
			return "ERROR unresolved " + t + "\n" + r.note;
		}
		Listing listing = currentProgram.getListing();
		SymbolTable st = currentProgram.getSymbolTable();
		StringBuilder sb = new StringBuilder(r.note);
		InstructionIterator it;
		if (r.fn != null) {
			sb.append("// ").append(r.fn.getEntryPoint()).append(' ').append(r.fn.getSymbol().getName(true)).append('\n');
			it = listing.getInstructions(r.fn.getBody(), true);
		}
		else {
			it = listing.getInstructions(r.addr, true);
		}
		int n = 0;
		while (it.hasNext() && n < max) {
			Instruction ins = it.next();
			byte[] b;
			try {
				b = ins.getBytes();
			}
			catch (MemoryAccessException e) {
				b = new byte[0];
			}
			sb.append(ins.getAddress()).append("  ").append(String.format("%-36s", hex(b))).append("  ").append(ins);
			for (Reference ref : ins.getReferencesFrom()) {
				Address to = ref.getToAddress();
				Symbol s = st.getPrimarySymbol(to);
				Data d = listing.getDataAt(to);
				if (d != null && d.hasStringValue()) {
					sb.append("    ; \"").append(trunc(esc(String.valueOf(d.getValue())), 100)).append('"');
				}
				else if (s != null) {
					sb.append("    ; ").append(s.getName(true));
				}
			}
			sb.append('\n');
			n++;
		}
		if (it.hasNext()) {
			sb.append("... truncated at ").append(max).append(" instructions\n");
		}
		return sb.toString();
	}

	private String xrefs(String t) {
		Resolved r = resolve(t);
		if (r.addr == null) {
			return "ERROR unresolved " + t + "\n" + r.note;
		}
		Address a = r.addr;
		boolean byName = !t.trim().toLowerCase(Locale.ROOT).startsWith("0x");
		if (r.fn != null && byName) {
			a = r.fn.getEntryPoint();
		}
		FunctionManager fm = currentProgram.getFunctionManager();
		StringBuilder sb = new StringBuilder(r.note).append("xrefs to ").append(a).append('\n');
		ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
		int n = 0;
		while (it.hasNext()) {
			Reference ref = it.next();
			Function f = fm.getFunctionContaining(ref.getFromAddress());
			sb.append(ref.getFromAddress()).append('\t').append(ref.getReferenceType()).append('\t')
					.append(fname(f)).append('\n');
			if (++n >= 3000) {
				sb.append("... truncated at 3000\n");
				break;
			}
		}
		if (n == 0) {
			sb.append("(none)\n");
		}
		return sb.toString();
	}

	private String callers(String t) {
		Resolved r = resolve(t);
		if (r.fn == null) {
			return "ERROR no function for " + t + "\n" + r.note;
		}
		FunctionManager fm = currentProgram.getFunctionManager();
		StringBuilder sb = new StringBuilder(r.note);
		sb.append("callers of ").append(fname(r.fn)).append('\n');
		ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(r.fn.getEntryPoint());
		int n = 0;
		while (it.hasNext()) {
			Reference ref = it.next();
			Function f = fm.getFunctionContaining(ref.getFromAddress());
			sb.append(ref.getFromAddress()).append('\t').append(ref.getReferenceType()).append('\t')
					.append(fname(f)).append('\n');
			if (++n >= 3000) {
				sb.append("... truncated at 3000\n");
				break;
			}
		}
		if (n == 0) {
			sb.append("(no direct references; may be called virtually - check vftables.tsv)\n");
		}
		return sb.toString();
	}

	private String callees(String t) {
		Resolved r = resolve(t);
		if (r.fn == null) {
			return "ERROR no function for " + t + "\n" + r.note;
		}
		StringBuilder sb = new StringBuilder(r.note);
		sb.append("callees of ").append(fname(r.fn)).append('\n');
		TreeSet<String> lines = new TreeSet<>();
		for (Function c : r.fn.getCalledFunctions(TaskMonitor.DUMMY)) {
			lines.add(c.isExternal() ? "EXT\t" + extName(c) : fname(c));
		}
		for (String l : lines) {
			sb.append(l).append('\n');
		}
		return sb.toString();
	}

	private String strings(String regex) {
		Pattern p = Pattern.compile(regex, Pattern.CASE_INSENSITIVE);
		FunctionManager fm = currentProgram.getFunctionManager();
		StringBuilder sb = new StringBuilder();
		DefinedStringIterator it = DefinedStringIterator.forProgram(currentProgram);
		int n = 0;
		while (it.hasNext()) {
			Data d = it.next();
			Object v = d.getValue();
			if (v == null) {
				continue;
			}
			String s = v.toString();
			if (!p.matcher(s).find()) {
				continue;
			}
			sb.append(d.getAddress()).append("\t\"").append(trunc(esc(s), 240)).append("\"\n");
			ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(d.getAddress());
			int k = 0;
			while (ri.hasNext() && k < 60) {
				Reference ref = ri.next();
				sb.append("    <- ").append(ref.getFromAddress()).append('\t')
						.append(fname(fm.getFunctionContaining(ref.getFromAddress()))).append('\n');
				k++;
			}
			if (++n >= 400) {
				sb.append("... truncated at 400 strings\n");
				break;
			}
		}
		if (n == 0) {
			sb.append("(no defined string matches; try exports/strings TSV or bytes search)\n");
		}
		return sb.toString();
	}

	private String symbols(String regex) {
		Pattern p = Pattern.compile(regex, Pattern.CASE_INSENSITIVE);
		StringBuilder sb = new StringBuilder();
		SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
		int n = 0;
		while (it.hasNext()) {
			Symbol s = it.next();
			String name = s.getName(true);
			if (!p.matcher(name).find()) {
				continue;
			}
			sb.append(s.getAddress()).append('\t').append(s.getSymbolType()).append('\t').append(name).append('\n');
			if (++n >= 600) {
				sb.append("... truncated at 600\n");
				break;
			}
		}
		return sb.toString();
	}

	private String bytes(String addr, int n) throws Exception {
		Resolved r = resolve(addr);
		if (r.addr == null) {
			return "ERROR unresolved " + addr;
		}
		byte[] b = new byte[Math.min(n, 4096)];
		int got = currentProgram.getMemory().getBytes(r.addr, b);
		StringBuilder sb = new StringBuilder();
		for (int i = 0; i < got; i += 16) {
			sb.append(r.addr.add(i)).append("  ").append(hex(Arrays.copyOfRange(b, i, Math.min(got, i + 16)))).append('\n');
		}
		return sb.toString();
	}

	private String qwords(String addr, int n) throws Exception {
		Resolved r = resolve(addr);
		if (r.addr == null) {
			return "ERROR unresolved " + addr;
		}
		Memory mem = currentProgram.getMemory();
		SymbolTable st = currentProgram.getSymbolTable();
		FunctionManager fm = currentProgram.getFunctionManager();
		StringBuilder sb = new StringBuilder();
		for (int i = 0; i < Math.min(n, 1024); i++) {
			Address at = r.addr.add(8L * i);
			long v = mem.getLong(at);
			sb.append(at).append("  ").append(String.format("%016x", v));
			try {
				Address to = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v);
				Symbol s = st.getPrimarySymbol(to);
				Function f = fm.getFunctionAt(to);
				if (f != null) {
					sb.append("  fn ").append(f.getSymbol().getName(true));
				}
				else if (s != null) {
					sb.append("  ").append(s.getName(true));
				}
			}
			catch (Exception e) {
				// not an address
			}
			sb.append('\n');
		}
		return sb.toString();
	}

	// ---------------------------------------------------------------- bulk export

	private void bulkExport() {
		try {
			log("bulk export start");
			FunctionManager fm = currentProgram.getFunctionManager();
			SymbolTable st = currentProgram.getSymbolTable();
			Memory mem = currentProgram.getMemory();

			try (PrintWriter w = pw("functions.tsv")) {
				w.println("entry\tname\tsize\tthunk\tprototype");
				for (Function f : fm.getFunctions(true)) {
					w.println(f.getEntryPoint() + "\t" + f.getSymbol().getName(true) + "\t" +
						f.getBody().getNumAddresses() + "\t" + (f.isThunk() ? "T" : "") + "\t" +
						esc(f.getPrototypeString(false, true)));
				}
			}
			log("functions.tsv done");

			try (PrintWriter w = pw("string_xrefs.tsv")) {
				w.println("string_addr\tstring\tfrom_addr\tfunc_entry\tfunc_name");
				DefinedStringIterator it = DefinedStringIterator.forProgram(currentProgram);
				while (it.hasNext()) {
					Data d = it.next();
					Object v = d.getValue();
					if (v == null) {
						continue;
					}
					String s = trunc(esc(v.toString()), 400);
					ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(d.getAddress());
					boolean any = false;
					while (ri.hasNext()) {
						Reference ref = ri.next();
						w.println(d.getAddress() + "\t" + s + "\t" + ref.getFromAddress() + "\t" +
							fname(fm.getFunctionContaining(ref.getFromAddress())));
						any = true;
					}
					if (!any) {
						w.println(d.getAddress() + "\t" + s + "\t-\t-\t-");
					}
				}
			}
			log("string_xrefs.tsv done");

			try (PrintWriter w = pw("import_xrefs.tsv")) {
				w.println("import\tcallsite\tfunc_entry\tfunc_name");
				for (Function ext : fm.getExternalFunctions()) {
					String name = extName(ext);
					Set<String> seen = new HashSet<>();
					for (Reference ref : ext.getSymbol().getReferences()) {
						Address from = ref.getFromAddress();
						Function f = fm.getFunctionContaining(from);
						if (f != null && !f.isThunk()) {
							if (seen.add(from.toString())) {
								w.println(name + "\t" + from + "\t" + fname(f));
							}
							continue;
						}
						Address slot = (f != null) ? f.getEntryPoint() : from;
						ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(slot);
						while (ri.hasNext()) {
							Reference r2 = ri.next();
							Function g = fm.getFunctionContaining(r2.getFromAddress());
							if (g != null && seen.add(r2.getFromAddress().toString())) {
								w.println(name + "\t" + r2.getFromAddress() + "\t" + fname(g));
							}
						}
					}
				}
			}
			log("import_xrefs.tsv done");

			try (PrintWriter w = pw("vftables.tsv")) {
				w.println("vftable_addr\tvftable_name\tindex\ttarget\ttarget_name");
				SymbolIterator it = st.getAllSymbols(true);
				while (it.hasNext()) {
					Symbol s = it.next();
					if (!s.getName().contains("vftable")) {
						continue;
					}
					for (int i = 0; i < 512; i++) {
						Address at = s.getAddress().add(8L * i);
						long v;
						try {
							v = mem.getLong(at);
						}
						catch (Exception e) {
							break;
						}
						Function f;
						try {
							f = fm.getFunctionAt(currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v));
						}
						catch (Exception e) {
							f = null;
						}
						if (f == null) {
							break;
						}
						w.println(s.getAddress() + "\t" + s.getName(true) + "\t" + i + "\t" + f.getEntryPoint() +
							"\t" + f.getSymbol().getName(true));
					}
				}
			}
			log("vftables.tsv done");

			try (PrintWriter w = pw("callgraph.tsv")) {
				w.println("caller_entry\tcallee");
				for (Function f : fm.getFunctions(true)) {
					for (Function c : f.getCalledFunctions(TaskMonitor.DUMMY)) {
						w.println(f.getEntryPoint() + "\t" + (c.isExternal() ? "EXT:" + extName(c) : c.getEntryPoint().toString()));
					}
				}
			}
			log("callgraph.tsv done");

			write(new File(exportsDir, "DONE"), "ok\n");
			log("bulk export done");
		}
		catch (Throwable e) {
			StringWriter sw = new StringWriter();
			e.printStackTrace(new PrintWriter(sw));
			log("bulk export failed " + e + "\n" + sw);
		}
	}

	// ---------------------------------------------------------------- helpers

	private String extName(Function ext) {
		ExternalLocation loc = ext.getExternalLocation();
		String lib = (loc != null && loc.getLibraryName() != null) ? loc.getLibraryName() : "?";
		return lib + "::" + ext.getName();
	}

	private PrintWriter pw(String name) throws IOException {
		return new PrintWriter(new OutputStreamWriter(
			new FileOutputStream(new File(exportsDir, name)), StandardCharsets.UTF_8));
	}

	private static void write(File f, String s) throws IOException {
		Files.write(f.toPath(), s.getBytes(StandardCharsets.UTF_8));
	}

	private synchronized void log(String s) {
		if (logw != null) {
			logw.println(java.time.LocalDateTime.now() + " " + s);
		}
	}

	private static String hex(byte[] b) {
		StringBuilder sb = new StringBuilder();
		for (byte x : b) {
			sb.append(String.format("%02x ", x & 0xff));
		}
		return sb.toString().trim();
	}

	private static String esc(String s) {
		return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n");
	}

	private static String trunc(String s, int n) {
		return s.length() <= n ? s : s.substring(0, n) + "…";
	}
}
