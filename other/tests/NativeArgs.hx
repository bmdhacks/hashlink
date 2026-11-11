class NativeArgs {
	@:hlNative("native_args","sum10") static function sum10(a:Int,b:Int,c:Int,d:Int,e:Int,f:Int,g:Int,h:Int,i:Int,j:Int):Int return 0;
	@:hlNative("native_args","sum12") static function sum12(a:Int,b:Int,c:Int,d:Int,e:Int,f:Int,g:Int,h:Int,i:Int,j:Int,k:Int,l:Int):Int return 0;
	@:hlNative("native_args","mixed") static function mixed(a:Int,b:Int,c:Int,d:Int,e:Int,f:Int,g:Int,h:Int,p9:Bool,p10:Int,p11:hl.UI8,p12:hl.UI16,p13:Float,p14:hl.F32,p15:hl.Bytes,a1:Float,a2:Float,a3:Float,a4:Float,a5:Float,a6:Float,a7:Float,a8:Float,fs9:Float,fs10:hl.F32,p16:Int):Float return 0;
	@:hlNative("native_args","dyn9") static function dyn9(a:Int,b:Int,c:Int,d:Int,e:Int,f:Int,g:Int,h:Int,i:Dynamic):Dynamic return null;
	static function check( name : String, v : Dynamic, e : Dynamic ) {
		if( v == e ) Sys.println("OK "+name+" "+v) else { Sys.println("FAIL "+name+" got "+v+" expected "+e); failed = true; }
	}
	static var failed = false;
	static function hlcall9(a:Int,b:Int,c:Int,d:Int,e:Int,f:Int,g:Int,h:Int,i:hl.UI8):Float
		return a+b+c+d+e+f+g+h+i*2.;
	static function hlcall(a:Int,b:Int,c:Int,d:Int,e:Int,f:Int,g:Int,h:Int,i:hl.UI8,j:Int,k:Float,l:hl.F32):Float
		return a+b+c+d+e+f+g+h+i*2.+j*3.+k*4.+l*5.;
	static function main() {
		// direct native call
		check("sum10", sum10(1,2,3,4,5,6,7,8,9,10), 1+4+9+16+25+36+49+64+81+100);
		check("sum12", sum12(1,2,3,4,5,6,7,8,1,2,3,4), 36+100+2000+30000+400000);
		var by = haxe.io.Bytes.alloc(1);
		check("mixed", mixed(1,2,3,4,5,6,7,8,true,10,11,12,13.5,14.5,by,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,2.5,0.25,16),
			36. + 1000. + 20. + 33. + 48. + 67.5 + 87. + 7. + 4. + 250. + 250. + 176.);
		// native called from a closure (HL call convention must be the native one)
		var cl = sum10;
		check("closure", cl(1,2,3,4,5,6,7,8,9,10), 385);
		// HL to HL with stack args
		check("hlcall", hlcall(1,2,3,4,5,6,7,8,9,10,1.5,2.5), 36. + 18. + 30. + 6. + 12.5);
		// dynamic calls are limited to 9 args
		var f : Dynamic = hlcall9;
		check("hlcall dyn", f(1,2,3,4,5,6,7,8,9), 36. + 18.);
		// through hl_dyn_call and back to HL stack args (c2hl and hl2c)
		var g : Dynamic = function(a:Int,b:Int,c:Int,d:Int,e:Int,f:Int,g:Int,h:Int,i:Int) return i*100 + a;
		check("reflect9", Reflect.callMethod(null,g,[1,2,3,4,5,6,7,8,9]), 901);
		var gp : (a:Int,b:Int,c:Int,d:Int,e:Int,f:Int,g:Int,h:Int,i:Dynamic) -> Dynamic = function(a,b,c,d,e,f,g,h,i) return i;
		var gd : Dynamic = gp;
		var obj = { v : 5 };
		var ret : Dynamic = Reflect.callMethod(null,gd,[1,2,3,4,5,6,7,8,obj]);
		check("reflect9-obj", ret.v, 5);
		// dynamic closure of a native with stack args through a wrapper
		var nd : Dynamic = dyn9;
		var r : Dynamic = nd(1,2,3,4,5,6,7,8,obj);
		check("native dyn9 closure", r.v, 5);
		if( failed ) Sys.exit(1);
	}
}
