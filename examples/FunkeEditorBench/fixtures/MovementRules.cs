public static class MovementRules
{
    public static ulong Step(ulong speed, ulong elapsed, ulong entity)
    {
        return (speed * elapsed) / 1000000ul + entity % 7ul;
    }
}
