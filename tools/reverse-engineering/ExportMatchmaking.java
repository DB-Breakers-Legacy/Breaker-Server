import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class ExportMatchmaking extends GhidraScript {
 public void run() throws Exception {
  String[] args=getScriptArgs();
  File dir=new File(args[0]); dir.mkdirs();
  DecompInterface decomp=new DecompInterface(); decomp.openProgram(currentProgram);
  FunctionIterator fs=currentProgram.getFunctionManager().getFunctions(true);
  int n=0;
  while(fs.hasNext() && !monitor.isCancelled()) {
   Function f=fs.next(); long a=f.getEntryPoint().getOffset();
   if(!((a>=0x140d60000L && a<0x140df0000L) || (a>=0x140500000L && a<0x140560000L) || (a>=0x140810000L && a<0x140820000L))) continue;
   DecompileResults r=decomp.decompileFunction(f,30,monitor);
   if(r.decompileCompleted()) {
    try(PrintWriter out=new PrintWriter(new File(dir,f.getEntryPoint()+".c"))) {out.println(r.getDecompiledFunction().getC());}
    n++;
   }
  }
  decomp.dispose(); println("Exported "+n+" SDK functions");
 }
}

