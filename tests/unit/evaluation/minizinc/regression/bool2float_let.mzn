% RUNS ON mzn20_fd
constraint
     let {
      var float: x = true
     }
     in
     x >= 0.0
;
solve satisfy;
