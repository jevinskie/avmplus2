/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package TestPackage
{
    public const num1 = 1;
    public const num2 = 2;
    
    public class Test
    {
        public function getNumber()
        {
            return num1 + num2;
        }
    }
}

// var SECTION = "Definitions\const";                  // provide a document reference (ie, ECMA section)
// var VERSION = "ActionScript 3.0";           // Version of JavaScript or ECMA
// var TITLE   = "using const in package in a class";       // Provide ECMA section title or a description
var BUGNUMBER = "";


import TestPackage.*;

import com.adobe.test.Assert;
var obj:Test = new Test();


Assert.expectEq("const in package with class", 3, obj.getNumber());

