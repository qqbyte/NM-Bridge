using System;

namespace TestLib
{
    public class Calculator
    {
        private string _name;

        public Calculator(string name)
        {
            _name = string.IsNullOrEmpty(name) ? "DefaultCalc" : name;
            Console.WriteLine($"[C# DLL] Create instance Calculator. Name: {_name}");
        }

        public static int Add(int a, int b)
        {
            Console.WriteLine($"[C# DLL] The static method Add was called.. Count: {a} + {b}");
            return a + b;
        }

        public string GetInfo()
        {
            Console.WriteLine($"[C# DLL] GetInfo method was called on the instance '{_name}'");
            return $"Calc Name: {_name} | Server time: {DateTime.Now:HH:mm:ss}";
        }
    }
}