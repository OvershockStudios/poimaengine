"""Pure expected-delta/fixture ownership checks; no native or compiler execution."""
import copy, unittest
import control_component_reads as v
class Predictions(unittest.TestCase):
    def payload(self):
        return dict(gameplay_revision=7,control_sequence=8,gameplay=dict(values=dict(Controls=3,Refusals=0),unchanged={'array':['9007199254740993',None]}),
            ui=dict(revision=9,elements={v.LABEL:dict(text='original',visible=True,enabled=True)}),
            entities=[{'id':'retained'}],components={'exact':['9223372036854775807']},future={'keep':[0,1.0,False]})
    def test_exact_refusal_delta_retains_every_other_field(self):
        before=self.payload();frozen=copy.deepcopy(before)
        result=v.expected_control(before,dict(Refusals=1),'full')
        self.assertEqual(before,frozen)
        self.assertEqual(result['gameplay']['values'],dict(Controls=4,Refusals=1))
        self.assertEqual((result['gameplay_revision'],result['control_sequence'],result['ui']['revision']),(8,9,10))
        self.assertEqual(result['ui']['elements'][v.LABEL],dict(text='full',visible=True,enabled=True))
        for key in ('entities','components','future'):self.assertTrue(v.exact(result[key],before[key]))
    def test_unpredicted_component_or_intent_change_is_not_discarded(self):
        expected=v.expected_control(self.payload(),dict(Refusals=1),'full')
        actual=copy.deepcopy(expected);actual['components']['exact'][0]='rounded'
        self.assertFalse(v.exact(actual,expected))
        actual=copy.deepcopy(expected);actual['gameplay']['values']['PendingIntent']=1
        self.assertFalse(v.exact(actual,expected))
    def test_json_number_type_change_is_not_equal(self):
        expected=v.expected_control(self.payload(),{},'read')
        actual=copy.deepcopy(expected);actual['future']['keep'][0]=0.0
        self.assertFalse(v.exact(actual,expected))
    def test_existing_world_rejected_before_authoring_transaction(self):
        class Author:
            calls=[]
            def call(self,method,params=None):self.calls.append(method);return dict(entity_count=3,revision=0)
        schemas=[dict(id=v.QUEUE,fields=[dict(kind='array',element_kind='entity',capacity=4,id='queue')]),
            dict(id=v.UNIT,fields=[dict(name=name,id=name) for name in ('Count','Exact','Single','Precise','Owner','Ints','Longs','Floats','Doubles','Entities')])]
        author=Author()
        with self.assertRaisesRegex(AssertionError,'actual empty'):v.prepare(author,schemas)
        self.assertEqual(author.calls,['world.inspect'])
if __name__=='__main__':unittest.main()
