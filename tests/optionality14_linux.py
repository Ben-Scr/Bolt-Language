import json, pathlib, subprocess, tempfile, os
import argparse
p=argparse.ArgumentParser(description='Linux-only real-process module startup comparison');p.add_argument('--bolt',required=True,type=pathlib.Path);p.add_argument('--output',type=pathlib.Path,default=pathlib.Path('optionality14.json'));args=p.parse_args();bolt=args.bolt.resolve();results={}
minimal='static class Program {public:void Main(){Print("READY14");Console.ReadLine();}}'
threads='''import System.Threading
class Job{public:Semaphore Signal;Job(Semaphore signal){Signal=signal;}void Run(){Signal.Wait(5000);}}
static class Program{public:void Main(){Semaphore signal(0,3);Thread a=Thread.Start(Job(signal)),b=Thread.Start(Job(signal)),c=Thread.Start(Job(signal));Print("READY14");Console.ReadLine();signal.Release(3);a.Join();b.Join();c.Join();}}'''
with tempfile.TemporaryDirectory(prefix='bolt-optional14-') as temp:
 root=pathlib.Path(temp)
 for name,text in [('minimal',minimal),('workers',threads)]:
  source=root/(name+'.bolt');source.write_text(text);exe=root/name
  build=subprocess.run([str(bolt),'compile',str(source),'--require-native','-o',str(exe),'--no-timings'],cwd=root,text=True,capture_output=True);assert build.returncode==0,build.stderr
  process=subprocess.Popen([str(exe)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  try:
   assert process.stdout.readline().strip()=='READY14'
   tasks=len(list(pathlib.Path(f'/proc/{process.pid}/task').iterdir()))
   sockets=[]
   for fd in pathlib.Path(f'/proc/{process.pid}/fd').iterdir():
    try:
     target=os.readlink(fd)
     if target.startswith('socket:'):sockets.append(target)
    except FileNotFoundError:pass
   process.stdin.write('\n');process.stdin.flush();out,err=process.communicate(timeout=15);assert process.returncode==0,(out,err)
   deps=subprocess.run(['readelf','-d',str(exe)],capture_output=True,text=True,check=True).stdout
   results[name]={'thread_count':tasks,'socket_fds':sockets,'executable_bytes':exe.stat().st_size,'needed_libraries':[line.strip() for line in deps.splitlines() if '(NEEDED)' in line]}
  finally:
   if process.poll() is None:process.kill();process.wait()
 assert results['minimal']['thread_count']==1 and not results['minimal']['socket_fds']
 assert results['workers']['thread_count']==4
results['scope']='Linux live-process comparison only; shared runtime still contains all module code and dependencies.'
args.output.write_text(json.dumps(results,indent=2));print(json.dumps(results,indent=2))
