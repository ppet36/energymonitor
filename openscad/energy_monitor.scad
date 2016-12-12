
innerWidth = 65.5;
innerHeight = 30.5;
innerDepth = 40;
thickness = 2;

$fn=100;

module box() {
  union() {
    difference() {
      cube ([innerWidth + 2*thickness, innerHeight+2*thickness, innerDepth + 2*thickness]);
      translate ([thickness,thickness,0]) cube ([innerWidth, innerHeight, innerDepth]);
      translate([innerWidth/2+thickness,(innerHeight+2*thickness)/2,(innerDepth/2)]) rotate([0,90,0]) cylinder (d=8,h=innerWidth+2*thickness,center=true);
    }
    
    color("red") translate([20,7,innerDepth+thickness]) rotate([0,0,90]) linear_extrude (height=3) text ("EM");
    color("red") translate([38,8,innerDepth+thickness]) rotate([0,0,90]) linear_extrude (height=3) text ("ioT");
  }
}

module ousko() {
  difference() {
    hull() {
      cylinder (d=15,h=5, center=true);
      translate([8,0,0]) cube ([15, 15, 5], center=true);
    }
    cylinder (d=4.2,h=5, center=true);
  }
}

module vrsek() {
  difference() {
    union() {
      box();
      translate ([-14,16.5,2.5]) ousko();
      translate ([innerWidth+14+thickness*2,16.5,2.5]) rotate ([0,0,180]) ousko();
    }
    vicko();
  }
}

module vicko() {
  color("green") {
    difference() {
      translate([thickness/2,thickness/2,0]) cube ([innerWidth + thickness-0.1, innerHeight+thickness-0.1, thickness]);
      translate([thickness/2+innerWidth/2,thickness/2+innerHeight/2+1,0])cylinder (d=12,h=thickness);
    }
  }
}

vrsek();
translate ([0,innerHeight+10,0]) vicko();

