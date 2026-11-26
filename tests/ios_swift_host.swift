import BoltInterpreter
import BoltScript

precondition(bolt_interpreter_abi() == 1)
precondition(bolt_mobile_run(0, nil) == 5)
print("PASS Swift imports both Bolt modules and calls the native iOS script")
