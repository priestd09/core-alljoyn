package org.alljoyn.bus.defs;

import java.util.ArrayList;
import java.util.List;

/**
 * Signal definition used to describe an interface signal.
 * Annotations commonly used: DocString, Deprecated, Sessionless, Sessioncast, Unicast, GlobalBroadcast.
 */
public class SignalDef extends BaseDef {

    final private String interfaceName;
    final private String signature;

    final private List<ArgDef> argList;


    /**
     * Constructor.
     *
     * @param name the name of the bus signal.
     * @param signature input parameter signature.
     * @param interfaceName the parent interface name.
     * @throws IllegalArgumentException one or more arguments is invalid.
     */
    public SignalDef(String name, String signature, String interfaceName) {
        super(name);
        if (signature == null) {
            throw new IllegalArgumentException("Null signature");
        }
        if (interfaceName == null) {
            throw new IllegalArgumentException("Null interfaceName");
        }
        this.interfaceName = interfaceName;
        this.signature = signature;
        this.argList = new ArrayList<ArgDef>();
    }

    /**
     * @return the name of the bus interface.
     */
    public String getInterfaceName() {
        return interfaceName;
    }

    /**
     * @return the input argument signature.
     */
    public String getSignature() {
        return signature;
    }

    /**
     * A signal has no return value (i.e. void return), which for an AllJoyn signature
     * is represented as an empty string. This method is provided as a convenience,
     * since it may not be obvious to clients that a void return is represented as
     * an empty string reply signature.
     *
     * @return the output signature, which is always an empty string.
     */
    public String getReplySignature() {
        return "";
    }

    /**
     * @return the signal's contained arg definitions.
     */
    public List<ArgDef> getArgList() {
        return argList;
    }

    public void setArgList( List<ArgDef> args ) {
        argList.clear();
        argList.addAll(args);
    }

    /**
     * Returns the arg def that matches the given arg name.
     *
     * @param name the arg name.
     * @return the matching arg def. Null if not found.
     */
    public ArgDef getArg(String name) {
        for (ArgDef arg : argList) {
            if (arg.getName().equals(name)) {
                return arg;
            }
        }
        return null;  // not found
    }

    /**
     * Add the given arg to the end of the arg list.
     *
     * @param arg the argument def to add.
     */
    public void addArg(ArgDef arg) {
        argList.add(arg);
    }

    /**
     * @return whether the bus signal is deprecated (indicated via a Deprecated annotation).
     */
    public boolean isDeprecated() {
        String value = getAnnotation(ANNOTATION_DEPRECATED);
        return Boolean.parseBoolean(value);
    }

    /**
     * @return whether the bus signal's emission behavior is sessionless (indicated via a Sessionless annotation).
     */
    public boolean isSessionless() {
        String value = getAnnotation(ANNOTATION_SIGNAL_SESSIONLESS);
        return Boolean.parseBoolean(value);
    }

    /**
     * @return whether the bus signal's emission behavior is sessioncast (indicated via a Sessioncast annotation).
     */
    public boolean isSessioncast() {
        String value = getAnnotation(ANNOTATION_SIGNAL_SESSIONCAST);
        return Boolean.parseBoolean(value);
    }

    /**
     * @return whether the bus signal's emission behavior is unicast (indicated via a Unicast annotation).
     */
    public boolean isUnicast() {
        String value = getAnnotation(ANNOTATION_SIGNAL_UNICAST);
        return Boolean.parseBoolean(value);
    }

    /**
     * @return whether the bus signal's emission behavior is global broadcast (indicated via a GlobalBroadcast annotation).
     */
    public boolean isGlobalBroadcast() {
        String value = getAnnotation(ANNOTATION_SIGNAL_GLOBAL_BROADCAST);
        return Boolean.parseBoolean(value);
    }

    @Override
    public String toString() {
        StringBuilder builder = new StringBuilder();
        builder.append("SignalDef {name=");
        builder.append(getName());
        builder.append(", signature=");
        builder.append(signature);
        builder.append(", interfaceName=");
        builder.append(interfaceName);
        builder.append("}");
        return builder.toString();
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (o == null || getClass() != o.getClass()) return false;

        SignalDef signalDef = (SignalDef) o;

        if (!interfaceName.equals(signalDef.interfaceName)) return false;
        if (!getName().equals(signalDef.getName())) return false;
        return signature.equals(signalDef.signature);

    }

    @Override
    public int hashCode() {
        int result = interfaceName.hashCode();
        result = 31 * result + getName().hashCode();
        result = 31 * result + signature.hashCode();
        return result;
    }

}
