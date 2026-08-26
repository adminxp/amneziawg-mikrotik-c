const fs=require('fs'),path=require('path'),{JSDOM}=require('jsdom');
const w=new JSDOM(fs.readFileSync(process.argv[2],'utf8'),{runScripts:'dangerously',url:'https://example.invalid/'}).window;
const o=JSON.parse(process.argv[4]);
process.stdout.write(w.buildUpdateScriptSource(process.argv[3],o));
