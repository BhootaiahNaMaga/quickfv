# Tcl package smoke test: the same loop an engineer would run in a Tcl shell.
# Every step is checked; any failure exits 1.
lappend auto_path [file join [file dirname [info script]] .. tcl]
package require qfv
set root [file normalize [file join [file dirname [info script]] ..]]
set ::env(QFV_BIN) [file join $root build qfv]
set tools [file join $root tools oss-cad-suite bin]
if {[file isdirectory $tools]} { set ::env(PATH) "$tools:$::env(PATH)" }

set failures 0
proc expect {what cond} {
    if {[uplevel 1 [list expr $cond]]} { puts "PASS  $what"; return }
    puts "FAIL  $what"
    incr ::failures
}

set work [file join $root work m4 tcl]
file delete -force [file join $work handoff.tcl]
qfv::start $work
set t [clock milliseconds]
set r [qfv::load [file join $root casestudy m4_session fifo_setup.tcl]]
puts "load: [expr {[clock milliseconds] - $t}] ms"
expect "load ok" {[regexp {"ok":true} $r]}
set r [qfv::check_assertion {wr_push && !rd_pop |=> !fifo_empty} -module fifo_1r1w_tb -name t_push -disable tb_reset -budget 2]
expect "t_push PASS_BOUNDED ([qfv::verdict $r t_push])" {[qfv::verdict $r t_push] eq "PASS_BOUNDED"}
set r [qfv::check_assertion {fifo_full && fifo_empty |-> rd_pop} -module fifo_1r1w_tb -name t_vac -disable tb_reset -budget 2]
expect "t_vac VACUOUS ([qfv::verdict $r t_vac])" {[qfv::verdict $r t_vac] eq "VACUOUS"}
set r [qfv::check_all 2]
expect "fifo_2 CEX ([qfv::verdict $r fifo_2])" {[qfv::verdict $r fifo_2] eq "CEX"}
set handoff [file join $work handoff.tcl]
qfv::export_jg $handoff
set text ""
if {[file exists $handoff]} {
    set f [open $handoff]
    set text [read $f]
    close $f
}
expect "handoff has the session assertion" {[string match "*assert -name t_push *" $text]}
expect "handoff proves what is open" {[string match "*prove -property*t_push*" $text]}
qfv::stop
puts [expr {$failures ? "FAILED: $failures" : "OK"}]
exit [expr {$failures ? 1 : 0}]
