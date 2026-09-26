# Tcl package smoke test: the same loop an engineer would run in a Tcl shell.
lappend auto_path [file join [file dirname [info script]] .. tcl]
package require qfv
set root [file normalize [file join [file dirname [info script]] ..]]
set ::env(QFV_BIN) [file join $root build qfv]
qfv::start [file join $root work m4 tcl]
set t [clock milliseconds]
set r [qfv::load [file join $root casestudy m4_session fifo_setup.tcl]]
puts "load: [expr {[clock milliseconds] - $t}] ms ok=[regexp {"ok":true} $r]"
set r [qfv::check_assertion {wr_push && !rd_pop |=> !fifo_empty} -module fifo_1r1w_tb -name t_push -disable tb_reset -budget 2]
puts "t_push: [qfv::verdict $r t_push]"
set r [qfv::check_assertion {fifo_full && fifo_empty |-> rd_pop} -module fifo_1r1w_tb -name t_vac -disable tb_reset -budget 2]
puts "t_vac:  [qfv::verdict $r t_vac]"
set r [qfv::check_all 2]
puts "fifo_2: [qfv::verdict $r fifo_2]"
qfv::export_jg [file join $root work m4 tcl handoff.tcl]
puts "handoff written: [file exists [file join $root work m4 tcl handoff.tcl]]"
qfv::stop
