package {
  var letters:Array = new Array("a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z");
  var numbers:Array = new Array(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26);
  var colors:Array  = new Array("FF","CC","99","66","33","00");

  var endResult:String;

  function doTest():void
  {
     endResult = "";

     // make up email address
     for (var k=0;k<4000;k++)
     {
        var name = makeName(6);
		var email = "";
        (k%2)?email=name+"@mac.com":email=name+"(at)mac.com";

        // validate the email address
        var pattern = /^[a-zA-Z0-9\-\._]+@[a-zA-Z0-9\-_]+(\.?[a-zA-Z0-9\-_]*)\.[a-zA-Z]{2,3}$/;

        if(pattern.test(email))
        {
           var r = email + " appears to be a valid email address.";
           addResult(r);
        }
        else
        {
           r = email + " does NOT appear to be a valid email address.";
           addResult(r);
        }
     }

     // make up ZIP codes
     for (var s=0;s<4000;s++)
     {
        var zipGood = true;
        var zip = makeNumber(4);
        (s%2)?zip=zip+"xyz":zip=zip + "7";

        // validate the zip code
        for (var i = 0; i < zip.length; i++) {
            var ch = zip.charAt(i);
            if (ch < "0" || ch > "9") {
                zipGood = false;
                r = zip + " contains letters.";
                addResult(r);
            }
        }
        if (zipGood && zip.length>5)
        {
           zipGood = false;
           r = zip + " is longer than five characters.";
           addResult(r);
        }
        if (zipGood)
        {
           r = zip + " appears to be a valid ZIP code.";
           addResult(r);
        }
     }
  }

  function makeName(n:int):String
  {
     var tmp = "";
     for (var i=0;i<n;i++)
     {
        var l = Math.floor(26*Math.random());
        tmp += letters[l];
     }
     return tmp;
  }

  function makeNumber(n:int):String
  {
     var tmp = "";
     for (var i=0;i<n;i++)
     {
        var l = Math.floor(9*Math.random());
        tmp = tmp + l;
     }
     return tmp;
  }

  function addResult(r:String):void
  {
     endResult += "\n" + r;
  }

  function runStringValidateInput():int {
        doTest();
    
    // verify test output - nothing concrete to verify, so make sure output length is correct.
    if (endResult.length != 462000) {
        print('Test verification failed.  Expected: 462000 Got: '+endResult.length);
    } 
  }

	for (var i = 0; i < 600; i++) {
  runStringValidateInput();
	}

}
