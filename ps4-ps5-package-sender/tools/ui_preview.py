"""Serve ui/ in a normal browser with a mocked window.__TAURI__ (design preview only)."""
import http.server, os, sys
UI = os.path.join(os.path.dirname(__file__), "..", "ui")
MOCK = r'''
<script>
(function(){
const GB=1073741824, now=Date.now();
const cfg={server_ip:"192.168.137.1",server_port:8339,auto_start_server:true,base_path:"D:\\PS4\\PKG",auto_scan:true,deep_scan:true,url_prefix:false,queue_scanner:true,ps_ip:"192.168.137.2",ps_app:"flatz",app_port:12800,poll_interval_ms:1500,max_parallel:1,delay_between_s:2,request_timeout_s:8,check_installed:false,send_icon:true,language:"en",accent:"blue",animations:true,glass:"light",confirm_exit:true,dragged_files:[]};
const L=(id,title,tid,kind,size,src,ver)=>({id,path:"D:\\PS4\\PKG\\"+title+".pkg",name:title.replace(/ /g,"_")+"_"+tid+".pkg",dir:"D:\\PS4\\PKG",size,title,title_id:tid,content_id:"EP0000-"+tid+"_00-XXXXXXXXXXXXXXXX",category:"gd",kind,app_ver:ver,sys_ver:"9.00",platform:"PS4",has_icon:false,source:src,url:"http://192.168.137.1:8339/"+id+"/x.pkg"});
const lib=[L("a1","Gravity Racer","CUSA10001","Game",38.2*GB,"base","01.00"),L("a2","Gravity Racer Update","CUSA10001","Patch",4.1*GB,"base","01.12"),L("a3","Neon Drift","CUSA20417","Game",12.7*GB,"base","01.00"),L("a4","Neon Drift Soundtrack","CUSA20417","DLC",0.4*GB,"base","01.00"),L("a5","Tiny Tactics","CUSA31120","Game",2.2*GB,"dragged","01.04")];
const Q=(qid,f,status,progress,speed,rest,err)=>({qid,file_id:f.id,path:f.path,name:f.name,title:f.title,title_id:f.title_id,content_id:f.content_id,kind:f.kind,size:f.size,has_icon:false,target:"flatz",status,progress,transferred:f.size*progress/100,rest_sec:rest,speed,task_id:status==="queued"?null:12,error:err||null,added_at:now,started_at:now,finished_at:null});
let queue=[Q(1,lib[0],"downloading",62.4,48.6*1048576,312),Q(2,lib[1],"queued",0,0,null),Q(3,lib[2],"completed",100,0,null),Q(4,lib[3],"failed",0,0,null,"error 0x80990085"),Q(5,lib[4],"paused",18.2,0,null)];
const server={running:true,port:8339,ip:"192.168.137.1",error:null,conflict:null,connections:2,speed:48.6*1048576,total_sent:20*GB};
const logs=[{seq:1,ts:now-60000,level:"info",msg:"PS4-PS5 Package Sender 1.0.0 started"},{seq:2,ts:now-59000,level:"info",msg:"Server listening on 0.0.0.0:8339"},{seq:3,ts:now-30000,level:"ok",msg:"Gravity Racer: install task 12 created"},{seq:4,ts:now-29000,level:"server",msg:"192.168.137.2 started download: Gravity_Racer_CUSA10001.pkg"}];
const listeners={};
const snap=()=>({cfg,server,queue,library:lib,scanning:false});
setInterval(()=>{queue[0].progress=Math.min(100,queue[0].progress+0.35);queue[0].rest_sec=Math.max(0,queue[0].rest_sec-1);(listeners.snapshot||[]).forEach(f=>f({payload:{cfg,server,queue,library:null,scanning:false}}));},1000);
window.__TAURI__={
 core:{invoke:async(c,a)=>{switch(c){case"get_snapshot":return snap();case"get_logs":return logs;case"list_interfaces":return[{name:"Ethernet",ip:"192.168.137.1"},{name:"Wi-Fi",ip:"192.168.1.23"}];case"get_icon":return null;case"test_connection":return"Remote Package Installer is responding";case"scan_network":await new Promise(r=>setTimeout(r,1200));return[{ip:"192.168.137.2",app:"flatz",label:"PS4 RPI (flatZ)"}];case"queue_autostart":return 1;case"queue_add":return 1;default:return null;}}},
 event:{listen:async(n,f)=>{(listeners[n]=listeners[n]||[]).push(f);return()=>{};}},
 window:{getCurrentWindow:()=>({minimize(){},toggleMaximize(){},close(){}})},
 app:{getVersion:async()=>"1.0.0"}};
window.__emit=(n,p)=>(listeners[n]||[]).forEach(f=>f({payload:p}));
})();
</script>
'''
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(s,*a,**k): super().__init__(*a,directory=UI,**k)
    def log_message(s,*a): pass
    def do_GET(s):
        if s.path in ("/","/index.html"):
            html=open(os.path.join(UI,"index.html"),encoding="utf-8").read().replace('<script src="app.js">',MOCK+'<script src="app.js">')
            b=html.encode(); s.send_response(200); s.send_header("Content-Type","text/html; charset=utf-8"); s.send_header("Content-Length",str(len(b))); s.end_headers(); s.wfile.write(b); return
        super().do_GET()
port=int(sys.argv[1]) if len(sys.argv)>1 else 5178
print("preview on",port,flush=True)
http.server.ThreadingHTTPServer(("127.0.0.1",port),H).serve_forever()
