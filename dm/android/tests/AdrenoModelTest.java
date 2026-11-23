package com.xiaoboluo.d2r;

// Adreno model parsing (two-digit names such as "Adreno22") and the Turnip legacy-numbering cut-off.
public class AdrenoModelTest {
    private static void equal(Object want, Object actual) {
        if (!want.equals(actual)) throw new AssertionError("want=" + want + " actual=" + actual);
    }
    public static void main(String[] args) {
        equal(750, AdrenoModel.of("Adreno750v2"));
        equal(22, AdrenoModel.of("Adreno22"));
        equal(619, AdrenoModel.of("Adreno 619"));
        equal(0, AdrenoModel.of(""));
        equal(0, AdrenoModel.of(null));
        equal(-1, AdrenoModel.of("Mali-G715"));
        equal(true, AdrenoModel.tooOldForTurnip(203));
        equal(true, AdrenoModel.tooOldForTurnip(505));
        equal(true, AdrenoModel.tooOldForTurnip(530));
        equal(true, AdrenoModel.tooOldForTurnip(599));
        equal(false, AdrenoModel.tooOldForTurnip(600));
        equal(false, AdrenoModel.tooOldForTurnip(619));
        equal(false, AdrenoModel.tooOldForTurnip(22));
        equal(false, AdrenoModel.tooOldForTurnip(99));
        equal(false, AdrenoModel.tooOldForTurnip(32));
        equal(false, AdrenoModel.tooOldForTurnip(750));
        equal(false, AdrenoModel.tooOldForTurnip(830));
        System.out.println("AdrenoModel OK (17 cases)");
    }
}
